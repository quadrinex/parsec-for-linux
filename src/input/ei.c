/* ei.c — libei backend (GNOME/KDE). UNTESTED: no portal dialog is triggered here.
 * Takes an EIS fd already obtained (RemoteDesktop.ConnectToEIS portal call done by the caller, or $LIBEI_SOCKET).
 * Not built by default: `make ei-check` (compile only). */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <libei.h>
#include "plh_input.h"

struct eib { plh_input base; struct ei *ei; struct ei_device *ptr, *abs_, *kbd; };

static uint64_t now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000000ull + t.tv_nsec / 1000; }

/* Drain events; keep devices when the server announces them, and start them. */
static void pump(struct eib *b)
{
    ei_dispatch(b->ei);
    struct ei_event *e;
    while ((e = ei_get_event(b->ei))) {
        switch (ei_event_get_type(e)) {
        case EI_EVENT_SEAT_ADDED:
            ei_seat_bind_capabilities(ei_event_get_seat(e), EI_DEVICE_CAP_POINTER, EI_DEVICE_CAP_POINTER_ABSOLUTE,
                                      EI_DEVICE_CAP_KEYBOARD, EI_DEVICE_CAP_BUTTON, EI_DEVICE_CAP_SCROLL, NULL);
            break;
        case EI_EVENT_DEVICE_ADDED: {
            struct ei_device *d = ei_event_get_device(e);
            if (ei_device_has_capability(d, EI_DEVICE_CAP_POINTER_ABSOLUTE)) b->abs_ = ei_device_ref(d);
            else if (ei_device_has_capability(d, EI_DEVICE_CAP_POINTER)) b->ptr = ei_device_ref(d);
            else if (ei_device_has_capability(d, EI_DEVICE_CAP_KEYBOARD)) b->kbd = ei_device_ref(d);
            break; }
        case EI_EVENT_DEVICE_RESUMED: ei_device_start_emulating(ei_event_get_device(e), 1); break;
        default: break;
        }
        ei_event_unref(e);
    }
}
static int frame(struct eib *b, struct ei_device *d) { if (!d) return -1; ei_device_frame(d, now_us()); pump(b); return 0; }

static int e_key(plh_input *in, int code, int down)
{ struct eib *b = (struct eib *)in; if (!b->kbd) return -1; ei_device_keyboard_key(b->kbd, code, down); return frame(b, b->kbd); }
static int e_rel(plh_input *in, int dx, int dy)
{ struct eib *b = (struct eib *)in; if (!b->ptr) return -1; ei_device_pointer_motion(b->ptr, dx, dy); return frame(b, b->ptr); }
static int e_abs(plh_input *in, int x, int y)
{ struct eib *b = (struct eib *)in; if (!b->abs_) return -1; ei_device_pointer_motion_absolute(b->abs_, in->desktop.x + x, in->desktop.y + y); return frame(b, b->abs_); }
static int e_btn(plh_input *in, enum plh_button bt, int down)
{
    static const int code[] = { 0x110, 0x111, 0x112, 0x113, 0x114 };   /* BTN_LEFT.. */
    struct eib *b = (struct eib *)in; struct ei_device *d = b->abs_ ? b->abs_ : b->ptr;
    if (!d) return -1; ei_device_button_button(d, code[bt], down); return frame(b, d);
}
static int e_wheel(plh_input *in, int v, int h)
{
    struct eib *b = (struct eib *)in; struct ei_device *d = b->ptr ? b->ptr : b->abs_;
    if (!d) return -1; ei_device_scroll_discrete(d, h, -v); return frame(b, d);
}
static void e_close(plh_input *in) { struct eib *b = (struct eib *)in; ei_unref(b->ei); free(b); }
static const struct plh_input_ops ei_ops = { "libei", e_key, e_rel, e_abs, e_btn, e_wheel, e_close };

plh_input *plh_ei_open_fd(int fd, const plh_rect *desktop, char *err, size_t errlen)
{
    struct eib *b = calloc(1, sizeof *b);
    if (!b) return NULL;
    b->base.ops = &ei_ops; b->base.desktop = *desktop;
    b->ei = ei_new_sender(NULL);
    if (!b->ei || ei_setup_backend_fd(b->ei, fd) < 0) { snprintf(err, errlen, "libei: connection refused"); free(b); return NULL; }
    ei_configure_name(b->ei, "parsec-for-linux");
    pump(b);
    return &b->base;
}
