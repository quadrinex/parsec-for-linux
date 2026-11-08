/* plh_input.h — input injection into the Linux desktop (uinput, libei backends…).
 *
 * Absolute coordinates: compositor desktop pixels (KWin logical space),
 * origin = top-left corner of the bounding rectangle of all screens.
 */
#ifndef PLH_INPUT_H
#define PLH_INPUT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct { int x, y, w, h; } plh_rect;

enum plh_button {
    PLH_BTN_LEFT = 0,
    PLH_BTN_RIGHT,
    PLH_BTN_MIDDLE,
    PLH_BTN_X1,     /* Windows XBUTTON1 (back)   -> BTN_SIDE  */
    PLH_BTN_X2,     /* Windows XBUTTON2 (forward) -> BTN_EXTRA */
    PLH_BTN_COUNT
};

typedef struct plh_input plh_input;

struct plh_input_ops {
    const char *name;
    int  (*key)(plh_input *in, int evcode, int down);          /* evdev KEY_* code */
    int  (*move_rel)(plh_input *in, int dx, int dy);
    int  (*move_abs)(plh_input *in, int x, int y);              /* desktop pixels */
    int  (*button)(plh_input *in, enum plh_button b, int down);
    int  (*wheel)(plh_input *in, int v120, int h120);           /* WHEEL_DELTA=120 units; v>0 = up, h>0 = right */
    void (*close)(plh_input *in);
};

struct plh_input {
    const struct plh_input_ops *ops;
    plh_rect desktop;
};

/* Native uinput backend. desktop = bounding rectangle of the desktop (NULL -> 0,0 5760x1080). */
plh_input *plh_uinput_open(const plh_rect *desktop, char *err, size_t errlen);
/* Backend libei (ei.c, untested, not built by default): EIS fd obtained from the RemoteDesktop portal. */
plh_input *plh_ei_open_fd(int fd, const plh_rect *desktop, char *err, size_t errlen);

/* Tests: same backend on arbitrary fds (pipes), no device created. */
plh_input *plh_uinput_wrap_fds(const plh_rect *desktop, int kbd, int rel, int abs_);

/* evdev node names ("eventN") of the 3 uinput devices (keyboard, relative, absolute). */
int plh_uinput_evnodes(plh_input *in, char kbd[16], char rel[16], char abs_[16]);

/* KWin (DBus session) : flat acceleration profile + speed 0 on device evnode.
 * Waits up to timeout_ms for KWin to discover it. 0 ok, <0 -errno. (kwin_dbus.c, -lsystemd) */
int plh_kwin_set_flat_accel(const char *evnode, int timeout_ms, char *err, size_t errlen);

/* Virtual Xbox 360 gamepad on uinput (pad.c). buttons = XINPUT_GAMEPAD_* bits; sticks -32768..32767
 * with Y up positive (XInput convention), triggers 0..255. Only changes are emitted. */
typedef struct plh_pad plh_pad;
plh_pad *plh_pad_create(int index, char *err, size_t errlen);
int  plh_pad_update(plh_pad *p, unsigned buttons, int lt, int rt, int lx, int ly, int rx, int ry);
void plh_pad_destroy(plh_pad *p);   /* removes the device */

/* Generic helpers (validate and delegate to the backend). Returns 0 ok, <0 -errno. */
int  plh_input_key_evdev(plh_input *in, int evcode, int down);
/* Windows set 1 scancode (KEYBDINPUT.wScan); extended = KEYEVENTF_EXTENDEDKEY (E0 prefix). */
int  plh_input_key_scancode(plh_input *in, unsigned scan, int extended, int down);
/* Windows virtual-key code (VK_*); extended tells e.g. keypad RETURN from main RETURN. */
int  plh_input_key_vk(plh_input *in, unsigned vk, int extended, int down);
int  plh_input_move_rel(plh_input *in, int dx, int dy);
int  plh_input_move_abs(plh_input *in, int x, int y);
int  plh_input_button(plh_input *in, enum plh_button b, int down);
int  plh_input_wheel(plh_input *in, int v120, int h120);
void plh_input_close(plh_input *in);

/* Tables (scancode.c). Returns 0 if unknown. */
int plh_scancode_to_evdev(unsigned scan, int extended);
int plh_vk_to_evdev(unsigned vk, int extended);
int plh_evdev_from_name(const char *name);   /* "KEY_F24", "F24", "LEFTSHIFT"... */

/* KWin layout (layout.c). */
struct plh_output { char name[32]; int x, y, w, h; int priority; };
/* Active outputs from "kscreen-doctor -o"; returns their count (<0 error). */
int plh_kwin_outputs(struct plh_output *o, int max);
int plh_parse_wlr_randr(const char *txt, struct plh_output *o, int max);
int plh_parse_xrandr(const char *txt, struct plh_output *o, int max);
int plh_parse_sway_json(const char *txt, struct plh_output *o, int max);
int plh_detect_outputs(struct plh_output *o, int max, const char **src);   /* auto: kwin > wlroots > X11 */
plh_rect plh_outputs_bounds(const struct plh_output *o, int n);
/* Translation T (KWin = Windows + T) matching each Windows monitor with a KWin output
 * of the same size; if ambiguous, prefers the T that puts the Windows primary on the KWin output
 * with priority 1. 0 ok, -1 none. */
int plh_layout_translation(const plh_rect *win, int nw, int win_primary,
                           const struct plh_output *k, int nk, int *tx, int *ty);
/* Fallback: T = position of the primary KWin output (priority 1). */
int plh_kwin_primary_translation(const struct plh_output *k, int nk, int *tx, int *ty);

/* The bridge's virtual output ("Virtual-plh-virtual"): counted in the desktop rectangle,
 * never used for the primary / translation search. */
int plh_output_is_virtual(const char *name);
/* Desktop bounding rect (all outputs, virtual included) + default translation (real primary). 0 ok. */
int plh_layout_snapshot(const struct plh_output *k, int nk, plh_rect *desk, int *tx, int *ty);

/* The KWin primary moved from old_d to new_d: the Windows->KWin translation moves by the same delta. */
void plh_translation_shift(int *tx, int *ty, int old_dx, int old_dy, int new_dx, int new_dy);

/* Redirect table: a Windows rect shown on another KWin rect (virtual output). */
struct plh_redir { plh_rect from, to; };
/* Maps a Windows point through the first entry containing it (scaled); 1 mapped, 0 no entry. */
int plh_redirect_map(const struct plh_redir *r, int n, int wx, int wy, int *kx, int *ky);

/* Absolute pointer device follows the desktop rect. 0 done/unchanged, 1 deferred (a button is
 * down; applied when the last one is released), <0 -errno. Keyboard and relative devices untouched. */
int plh_uinput_resize(plh_input *in, const plh_rect *rect);
/* Microseconds the last recreation took (create new device, destroy old); 0 if none. */
long plh_uinput_last_resize_us(plh_input *in);

/* SendInput MOUSEEVENTF_ABSOLUTE (0..65535) -> pixels.
 * vrect = reference rectangle (Windows virtual screen if VIRTUALDESK, else primary screen),
 * in Windows space; result in Windows space (translate afterwards). */
void plh_norm65535_to_px(const plh_rect *vrect, int nx, int ny, int *px, int *py);

#ifdef __cplusplus
}
#endif
#endif
