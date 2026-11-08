/* uinput.c — native uinput backend: 3 virtual devices
 *   - "PLH virtual keyboard"          : all KEY_* keys
 *   - "PLH virtual mouse"             : REL_X/Y, wheels (+ HI_RES), 5 buttons
 *   - "PLH virtual absolute pointer"  : ABS_X 0..w-1, ABS_Y 0..h-1, 5 buttons, wheels
 * The absolute pointer has NEITHER INPUT_PROP_DIRECT NOR BTN_TOOL_*: udev classes it ID_INPUT_MOUSE
 * (like QEMU's USB tablet), libinput treats it as an absolute pointer and KWin maps it
 * onto the bounding rectangle of the desktop (checked by test, see tests/). */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/uinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include "plh_input.h"

struct uin {
    plh_input base;
    int kbd, rel, abs;
    int last_ptr;           /* fd of the last pointer that moved (buttons/wheel) */
    int wacc_v, wacc_h;     /* wheel remainder < 120 */
    int ax, ay;             /* last ABS values sent (-1 = none) */
    int owned;              /* fds are real uinput devices (else test pipes) */
    unsigned btn_down;      /* bitmask of buttons held through this backend */
    int has_pending;        /* resize deferred while a button is down */
    plh_rect pending;
    long resize_us;
};

static const int btn_codes[PLH_BTN_COUNT] = { BTN_LEFT, BTN_RIGHT, BTN_MIDDLE, BTN_SIDE, BTN_EXTRA };

static int emit(int fd, int type, int code, int val)
{
    struct input_event ev;
    memset(&ev, 0, sizeof ev);
    ev.type = (unsigned short)type;
    ev.code = (unsigned short)code;
    ev.value = val;
    if (write(fd, &ev, sizeof ev) != (ssize_t)sizeof ev) return -errno;
    return 0;
}
static int syn(int fd) { return emit(fd, EV_SYN, SYN_REPORT, 0); }

static int mkdev(const char *name, unsigned short pid, int kind, const plh_rect *d, char *err, size_t errlen)
{
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) { snprintf(err, errlen, "open /dev/uinput: %s", strerror(errno)); return -1; }
#define IO(req, arg) do { if (ioctl(fd, req, arg) < 0) { snprintf(err, errlen, #req ": %s", strerror(errno)); close(fd); return -1; } } while (0)
    if (kind == 0) {
        IO(UI_SET_EVBIT, EV_KEY);
        IO(UI_SET_EVBIT, EV_REP);
        for (int k = 1; k < 256; k++) IO(UI_SET_KEYBIT, k);   /* KEY_ESC..KEY_MICMUTE-ish */
    } else {
        IO(UI_SET_EVBIT, EV_KEY);
        for (int i = 0; i < PLH_BTN_COUNT; i++) IO(UI_SET_KEYBIT, btn_codes[i]);
        IO(UI_SET_EVBIT, EV_REL);
        IO(UI_SET_RELBIT, REL_WHEEL);
        IO(UI_SET_RELBIT, REL_HWHEEL);
        IO(UI_SET_RELBIT, REL_WHEEL_HI_RES);
        IO(UI_SET_RELBIT, REL_HWHEEL_HI_RES);
        if (kind == 1) {
            IO(UI_SET_RELBIT, REL_X);
            IO(UI_SET_RELBIT, REL_Y);
            IO(UI_SET_PROPBIT, INPUT_PROP_POINTER);
        } else {
            IO(UI_SET_EVBIT, EV_ABS);
            IO(UI_SET_ABSBIT, ABS_X);
            IO(UI_SET_ABSBIT, ABS_Y);
            struct uinput_abs_setup a;
            memset(&a, 0, sizeof a);
            a.code = ABS_X; a.absinfo.minimum = 0; a.absinfo.maximum = d->w - 1;
            IO(UI_ABS_SETUP, &a);
            a.code = ABS_Y; a.absinfo.minimum = 0; a.absinfo.maximum = d->h - 1;
            IO(UI_ABS_SETUP, &a);
        }
    }
    struct uinput_setup us;
    memset(&us, 0, sizeof us);
    us.id.bustype = BUS_VIRTUAL;
    us.id.vendor = 0x1d6b;   /* Linux Foundation, like usual virtual devices */
    us.id.product = pid;
    us.id.version = 1;
    snprintf(us.name, sizeof us.name, "%s", name);
    IO(UI_DEV_SETUP, &us);
    IO(UI_DEV_CREATE, 0);
#undef IO
    return fd;
}

static int u_key(plh_input *in, int code, int down)
{
    struct uin *u = (struct uin *)in;
    int r = emit(u->kbd, EV_KEY, code, down ? 1 : 0);
    return r ? r : syn(u->kbd);
}

static int u_rel(plh_input *in, int dx, int dy)
{
    struct uin *u = (struct uin *)in;
    int r = 0;
    if (dx) r = emit(u->rel, EV_REL, REL_X, dx);
    if (!r && dy) r = emit(u->rel, EV_REL, REL_Y, dy);
    u->last_ptr = u->rel;
    return r ? r : syn(u->rel);
}

static int u_abs(plh_input *in, int x, int y)
{
    struct uin *u = (struct uin *)in;
    int vx = x - in->desktop.x, vy = y - in->desktop.y, r = 0;
    /* The kernel drops an ABS_X/ABS_Y equal to the last value: if the cursor was
     * moved in between (relative, real mouse), a return to the same point would be lost.
     * So go through a neighbour (1 px) to force the update. */
    if (vx == u->ax && vy == u->ay) {
        int jx = vx > 0 ? vx - 1 : vx + 1;
        r = emit(u->abs, EV_ABS, ABS_X, jx);
        if (!r) r = syn(u->abs);
    }
    if (!r) r = emit(u->abs, EV_ABS, ABS_X, vx);
    if (!r) r = emit(u->abs, EV_ABS, ABS_Y, vy);
    u->ax = vx; u->ay = vy;
    u->last_ptr = u->abs;
    return r ? r : syn(u->abs);
}

static int u_button(plh_input *in, enum plh_button b, int down)
{
    struct uin *u = (struct uin *)in;
    int r = emit(u->last_ptr, EV_KEY, btn_codes[b], down ? 1 : 0);
    if (down) u->btn_down |= 1u << b; else u->btn_down &= ~(1u << b);
    r = r ? r : syn(u->last_ptr);
    if (!u->btn_down && u->has_pending) { u->has_pending = 0; plh_uinput_resize(in, &u->pending); }
    return r;
}

static int u_wheel(plh_input *in, int v120, int h120)
{
    struct uin *u = (struct uin *)in;
    int fd = u->last_ptr, r = 0;
    if (v120) {
        u->wacc_v += v120;
        int notches = u->wacc_v / 120;
        u->wacc_v -= notches * 120;
        r = emit(fd, EV_REL, REL_WHEEL_HI_RES, v120);
        if (!r && notches) r = emit(fd, EV_REL, REL_WHEEL, notches);
    }
    if (!r && h120) {
        u->wacc_h += h120;
        int notches = u->wacc_h / 120;
        u->wacc_h -= notches * 120;
        r = emit(fd, EV_REL, REL_HWHEEL_HI_RES, h120);
        if (!r && notches) r = emit(fd, EV_REL, REL_HWHEEL, notches);
    }
    return r ? r : syn(fd);
}

static void u_close(plh_input *in)
{
    struct uin *u = (struct uin *)in;
    int fds[3] = { u->kbd, u->rel, u->abs };
    for (int i = 0; i < 3; i++)
        if (fds[i] >= 0) { ioctl(fds[i], UI_DEV_DESTROY); close(fds[i]); }   /* ioctl has no effect on a pipe */
    free(u);
}

/* evdev node name ("eventN") of a created uinput device. */
static int evnode_of(int fd, char *out, size_t len)
{
    char sys[64], dir[160];
    if (ioctl(fd, UI_GET_SYSNAME(sizeof sys), sys) < 0) return -errno;
    snprintf(dir, sizeof dir, "/sys/devices/virtual/input/%s", sys);
    DIR *d = opendir(dir);
    if (!d) return -errno;
    struct dirent *de;
    int r = -ENOENT;
    while ((de = readdir(d)))
        if (!strncmp(de->d_name, "event", 5)) { if (strlen(de->d_name) >= len) break; memcpy(out, de->d_name, strlen(de->d_name) + 1); r = 0; break; }
    closedir(d);
    return r;
}

int plh_uinput_evnodes(plh_input *in, char kbd[16], char rel[16], char abs_[16])
{
    struct uin *u = (struct uin *)in;
    int r = evnode_of(u->kbd, kbd, 16);
    if (!r) r = evnode_of(u->rel, rel, 16);
    if (!r) r = evnode_of(u->abs, abs_, 16);
    return r;
}

int plh_uinput_resize(plh_input *in, const plh_rect *rect)
{
    struct uin *u = (struct uin *)in;
    if (!in || !rect || rect->w <= 0 || rect->h <= 0) return -EINVAL;
    if (rect->x == in->desktop.x && rect->y == in->desktop.y && rect->w == in->desktop.w && rect->h == in->desktop.h) {
        u->has_pending = 0;
        return 0;
    }
    if (u->btn_down) { u->pending = *rect; u->has_pending = 1; return 1; }
    u->has_pending = 0;
    if (u->owned) {
        struct timespec t0, t1;
        char err[128];
        clock_gettime(CLOCK_MONOTONIC, &t0);
        /* new device first: if it fails the old one keeps working */
        int nfd = mkdev("PLH virtual absolute pointer", 0x7003, 2, rect, err, sizeof err);
        if (nfd < 0) return -EIO;
        if (u->last_ptr == u->abs) u->last_ptr = nfd;
        ioctl(u->abs, UI_DEV_DESTROY);
        close(u->abs);
        u->abs = nfd;
        clock_gettime(CLOCK_MONOTONIC, &t1);
        u->resize_us = (t1.tv_sec - t0.tv_sec) * 1000000L + (t1.tv_nsec - t0.tv_nsec) / 1000;
    }
    u->ax = u->ay = -1;
    in->desktop = *rect;
    return 0;
}

long plh_uinput_last_resize_us(plh_input *in) { return ((struct uin *)in)->resize_us; }

static const struct plh_input_ops uinput_ops = {
    "uinput", u_key, u_rel, u_abs, u_button, u_wheel, u_close
};

/* Tests : uinput backend on arbitrary fds (pipes), without creating a device. */
plh_input *plh_uinput_wrap_fds(const plh_rect *desktop, int kbd, int rel, int abs_)
{
    struct uin *u = calloc(1, sizeof *u);
    if (!u) return NULL;
    u->base.ops = &uinput_ops;
    u->base.desktop = *desktop;
    u->kbd = kbd; u->rel = rel; u->abs = abs_;
    u->ax = u->ay = -1;
    u->last_ptr = u->abs;
    return &u->base;
}

plh_input *plh_uinput_open(const plh_rect *desktop, char *err, size_t errlen)
{
    static const plh_rect def = { 0, 0, 5760, 1080 };
    struct uin *u = calloc(1, sizeof *u);
    if (!u) { snprintf(err, errlen, "calloc"); return NULL; }
    u->base.ops = &uinput_ops;
    u->base.desktop = desktop ? *desktop : def;
    u->kbd = u->rel = u->abs = -1;
    u->ax = u->ay = -1;
    if (u->base.desktop.w <= 0 || u->base.desktop.h <= 0) { snprintf(err, errlen, "empty desktop"); free(u); return NULL; }
    if ((u->kbd = mkdev("PLH virtual keyboard", 0x7001, 0, NULL, err, errlen)) < 0 ||
        (u->rel = mkdev("PLH virtual mouse", 0x7002, 1, NULL, err, errlen)) < 0 ||
        (u->abs = mkdev("PLH virtual absolute pointer", 0x7003, 2, &u->base.desktop, err, errlen)) < 0) {
        u_close(&u->base);
        return NULL;
    }
    u->last_ptr = u->abs;
    u->owned = 1;
    return &u->base;
}
