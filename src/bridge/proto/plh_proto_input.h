/*
 * plh_proto_input.h — bridge service 0x03 "input". Owner: input.
 * Included by plh_proto.h. Compiles as C11 native and mingw-w64 (fixed sizes, little-endian).
 *
 * Typical client: plh-input-hook.exe (PE, dedicated prefix) which observes the input INJECTED by
 * Parsec into the Wine desktop (public hooks WH_MOUSE_LL / WH_KEYBOARD_LL, *_INJECTED flag),
 * swallows it, and sends it here; the daemon replays it on the real desktop (uinput or libei).
 *
 * Coordinates: absolute positions are in pixels of the WINDOWS VIRTUAL SCREEN (Wine's,
 * primary at 0,0). The daemon translates them to KWin space (translation obtained by
 * PLH_MSG_INPUT_SET_LAYOUT, otherwise derived from the KWin primary screen).
 */
#ifndef PLH_PROTO_INPUT_H
#define PLH_PROTO_INPUT_H

#include <stdint.h>

#ifndef PLH_STATIC_ASSERT
#define PLH_STATIC_ASSERT(c, m) _Static_assert(c, m)
#endif

enum plh_msg_type_input {
    PLH_MSG_INPUT_EVENTS      = 0x0301, /* C->D plh_input_events_hdr + ev[count]; reply only if seq != 0 (plh_input_status) */
    PLH_MSG_INPUT_SET_LAYOUT  = 0x0302, /* C->D plh_input_layout_hdr + mon[count]; reply plh_input_layout_reply */
    PLH_MSG_INPUT_RELEASE_ALL = 0x0303, /* C->D (empty) releases keys/buttons held by this client; reply if seq != 0 */
    PLH_MSG_INPUT_GAMEPAD     = 0x0310, /* C->D plh_input_gamepad; no reply if seq == 0 */
};

enum plh_input_ev_type {
    PLH_IEV_KEY      = 1, /* code = set 1 scancode (0 if unknown), a = Windows VK, b = 1 press / 0 release; flags EXTENDED */
    PLH_IEV_MOVE_REL = 2, /* a = dx, b = dy (pixels, unaccelerated) */
    PLH_IEV_MOVE_ABS = 3, /* a = x, b = y (Windows virtual screen, pixels) */
    PLH_IEV_BUTTON   = 4, /* code = PLH_IBTN_*, b = 1 press / 0 release */
    PLH_IEV_WHEEL    = 5, /* a = vertical (WHEEL_DELTA=120, >0 = up), b = horizontal (>0 = right) */
};

#define PLH_IEV_F_EXTENDED 0x01 /* extended key (E0 prefix / KEYEVENTF_EXTENDEDKEY / LLKHF_EXTENDED) */
#define PLH_IEV_F_INFERRED 0x02 /* MOVE_REL inferred by the hook (pt - Wine cursor), sent with the MOVE_ABS
                                   of the same motion: the daemon applies one OR the other (mouse mode) */

enum plh_input_button_id {
    PLH_IBTN_LEFT = 0, PLH_IBTN_RIGHT = 1, PLH_IBTN_MIDDLE = 2, PLH_IBTN_X1 = 3, PLH_IBTN_X2 = 4,
};

struct plh_input_ev {
    uint8_t  type;      /* enum plh_input_ev_type */
    uint8_t  flags;     /* PLH_IEV_F_* */
    uint16_t code;
    int32_t  a;
    int32_t  b;
    uint32_t time_ms;   /* source timestamp (GetTickCount / KBDLLHOOKSTRUCT.time), informational */
};
PLH_STATIC_ASSERT(sizeof(struct plh_input_ev) == 16, "input_ev");

struct plh_input_events_hdr {
    uint32_t count;     /* number of plh_input_ev that follow (<= 4096) */
    uint32_t reserved;
};

struct plh_input_status {
    int32_t  status;    /* enum plh_status */
    uint32_t applied;   /* events applied */
};

/* Monitor layout as seen by Windows (EnumDisplayMonitors). */
struct plh_input_monitor {
    int32_t  x, y, w, h;
    uint32_t primary;   /* MONITORINFOF_PRIMARY */
    uint32_t reserved;
};
PLH_STATIC_ASSERT(sizeof(struct plh_input_monitor) == 24, "input_monitor");

struct plh_input_layout_hdr {
    uint32_t count;     /* <= 16 */
    uint32_t reserved;
};

struct plh_input_layout_reply {
    int32_t  status;    /* PLH_OK, or PLH_E_NOENT if no translation makes the layouts coincide */
    int32_t  tx, ty;    /* KWin = Windows + (tx, ty) */
    int32_t  desk_x, desk_y, desk_w, desk_h; /* KWin bounding rectangle */
    uint32_t backend;   /* 1 = uinput, 2 = libei */
};

/* Gamepad, XINPUT_GAMEPAD layout. */
struct plh_input_gamepad {
    uint32_t index;     /* 0..3 */
    uint32_t connected; /* 0 = unplug the virtual gamepad */
    uint16_t buttons;   /* XINPUT_GAMEPAD_* */
    uint8_t  left_trigger, right_trigger;
    int16_t  lx, ly, rx, ry;
    uint32_t reserved;
};
PLH_STATIC_ASSERT(sizeof(struct plh_input_gamepad) == 24, "input_gamepad");

#endif
