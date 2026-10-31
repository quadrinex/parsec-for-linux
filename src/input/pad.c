/* pad.c — virtual Xbox 360 gamepad on uinput (045e:028e), laid out like the kernel xpad driver:
 * sticks ABS_X/Y/RX/RY (-32768..32767, Y inverted: up is negative), triggers ABS_Z/RZ (0..255),
 * D-pad ABS_HAT0X/Y, buttons BTN_A/B/X/Y/TL/TR/SELECT/START/MODE/THUMBL/THUMBR.
 * Input is an XINPUT_GAMEPAD-style state; only changes are emitted. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/uinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "plh_input.h"

struct plh_pad {
    int fd;
    int btn[11], hx, hy, axis[6];
    int init;
};

/* XINPUT_GAMEPAD_* bit -> evdev button, in the order of struct plh_pad.btn */
static const struct { unsigned mask; int code; } btnmap[11] = {
    { 0x1000, BTN_A }, { 0x2000, BTN_B }, { 0x4000, BTN_X }, { 0x8000, BTN_Y },
    { 0x0100, BTN_TL }, { 0x0200, BTN_TR }, { 0x0020, BTN_SELECT }, { 0x0010, BTN_START },
    { 0x0400, BTN_MODE }, { 0x0040, BTN_THUMBL }, { 0x0080, BTN_THUMBR },
};
static const int axcode[6] = { ABS_X, ABS_Y, ABS_RX, ABS_RY, ABS_Z, ABS_RZ };

static int emit(int fd, int type, int code, int val)
{
    struct input_event ev;
    memset(&ev, 0, sizeof ev);
    ev.type = (unsigned short)type; ev.code = (unsigned short)code; ev.value = val;
    return write(fd, &ev, sizeof ev) == (ssize_t)sizeof ev ? 0 : -errno;
}

plh_pad *plh_pad_create(int index, char *err, size_t errlen)
{
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) { snprintf(err, errlen, "open /dev/uinput: %s", strerror(errno)); return NULL; }
#define IO(req, arg) do { if (ioctl(fd, req, arg) < 0) { snprintf(err, errlen, #req ": %s", strerror(errno)); close(fd); return NULL; } } while (0)
    IO(UI_SET_EVBIT, EV_KEY);
    for (int i = 0; i < 11; i++) IO(UI_SET_KEYBIT, btnmap[i].code);
    IO(UI_SET_EVBIT, EV_ABS);
    for (int i = 0; i < 6; i++) {
        struct uinput_abs_setup a;
        memset(&a, 0, sizeof a);
        IO(UI_SET_ABSBIT, axcode[i]);
        a.code = (unsigned short)axcode[i];
        if (i < 4) { a.absinfo.minimum = -32768; a.absinfo.maximum = 32767; a.absinfo.flat = 128; }
        else { a.absinfo.minimum = 0; a.absinfo.maximum = 255; }
        IO(UI_ABS_SETUP, &a);
    }
    for (int i = 0; i < 2; i++) {
        struct uinput_abs_setup a;
        memset(&a, 0, sizeof a);
        IO(UI_SET_ABSBIT, i ? ABS_HAT0Y : ABS_HAT0X);
        a.code = (unsigned short)(i ? ABS_HAT0Y : ABS_HAT0X); a.absinfo.minimum = -1; a.absinfo.maximum = 1;
        IO(UI_ABS_SETUP, &a);
    }
    struct uinput_setup us;
    memset(&us, 0, sizeof us);
    us.id.bustype = BUS_USB; us.id.vendor = 0x045e; us.id.product = 0x028e; us.id.version = 0x0110;
    snprintf(us.name, sizeof us.name, "Microsoft X-Box 360 pad (PLH %d)", index);
    IO(UI_DEV_SETUP, &us);
    IO(UI_DEV_CREATE, 0);
#undef IO
    plh_pad *p = calloc(1, sizeof *p);
    if (!p) { ioctl(fd, UI_DEV_DESTROY); close(fd); snprintf(err, errlen, "calloc"); return NULL; }
    p->fd = fd;
    return p;
}

int plh_pad_update(plh_pad *p, unsigned buttons, int lt, int rt, int lx, int ly, int rx, int ry)
{
    int r = 0, ch = 0;
    int ax[6] = { lx, -1 - ly, rx, -1 - ry, lt, rt };
    int hx = ((buttons & 0x8) ? 1 : 0) - ((buttons & 0x4) ? 1 : 0);
    int hy = ((buttons & 0x2) ? 1 : 0) - ((buttons & 0x1) ? 1 : 0);
    for (int i = 0; i < 11 && !r; i++) {
        int v = (buttons & btnmap[i].mask) != 0;
        if (v != p->btn[i] || !p->init) { p->btn[i] = v; r = emit(p->fd, EV_KEY, btnmap[i].code, v); ch = 1; }
    }
    for (int i = 0; i < 6 && !r; i++)
        if (ax[i] != p->axis[i] || !p->init) { p->axis[i] = ax[i]; r = emit(p->fd, EV_ABS, axcode[i], ax[i]); ch = 1; }
    if (!r && (hx != p->hx || !p->init)) { p->hx = hx; r = emit(p->fd, EV_ABS, ABS_HAT0X, hx); ch = 1; }
    if (!r && (hy != p->hy || !p->init)) { p->hy = hy; r = emit(p->fd, EV_ABS, ABS_HAT0Y, hy); ch = 1; }
    p->init = 1;
    return r ? r : (ch ? emit(p->fd, EV_SYN, SYN_REPORT, 0) : 0);
}

void plh_pad_destroy(plh_pad *p)
{
    if (!p) return;
    ioctl(p->fd, UI_DEV_DESTROY);
    close(p->fd);
    free(p);
}
