/*
 * plh_proto_vdisplay.h — virtual display messages. Owner: capture. Included by plh_proto_capture.h.
 *
 * The bridge is the source of truth for the settings (env PLH_VIRTUAL_DISPLAY, PLH_VIRTUAL_DISPLAY_AUTO,
 * PLH_VIRTUAL_DISPLAY_MODE); clients ask with DISPLAY_STATE and never read the environment.
 *
 * VOUT lifecycle (at most ONE virtual output in the daemon):
 *   C: VOUT_CREATE {w,h,refresh}  -> D: reply {status, output_id, KWin rect}. The output is then also in
 *      CAP_LIST_OUTPUTS with PLH_CAP_OUTPUT_VIRTUAL, and can be opened with CAP_OPEN like any output.
 *      A second create while one exists (from any client) returns the existing output (owners are refcounted).
 *   C: VOUT_DESTROY {output_id}   -> the output is removed after the last owner released AND any CAP stream
 *      on it is closed. Owners also lose it when their connection drops. The bridge exit removes it.
 *   Grace period: the DXGI proxy waits 3 s after the duplication Release before VOUT_DESTROY (client side).
 *
 * INPUT_REDIRECT (service 0x03, implemented by input, F2 only): table of Windows rect -> KWin rect applied
 * before the global Windows->KWin translation. Sent by the DXGI proxy; the table belongs to the sending
 * connection and is dropped when it closes. An empty table clears it.
 */
#ifndef PLH_PROTO_VDISPLAY_H
#define PLH_PROTO_VDISPLAY_H

#define PLH_MSG_INPUT_REDIRECT 0x0320 /* C->D plh_input_redirect_hdr + plh_redirect[count]; reply (if seq) empty */

#define PLH_VOUT_NAME        "plh-virtual"                       /* KWin shows "Virtual-plh-virtual" */
#define PLH_VOUT_DESCRIPTION "Virtual display (parsec-for-linux)"
#define PLH_VOUT_MAX_WIDTH   4096
#define PLH_VOUT_MAX_HEIGHT  2304 /* sizes are rounded down to even and clamped (encoder limit) */

struct plh_vout_create {
    uint32_t width, height;      /* 0 = configured mode (PLH_VIRTUAL_DISPLAY_MODE); width capped at 4096 */
    uint32_t refresh_mhz;        /* 0 = 60000 */
    uint32_t reserved;
};
PLH_STATIC_ASSERT(sizeof(struct plh_vout_create) == 16, "vout_create");

struct plh_vout_create_reply {
    int32_t  status;
    uint32_t output_id;          /* id in CAP_LIST_OUTPUTS / CAP_OPEN */
    int32_t  x, y;               /* KWin rect */
    uint32_t width, height;
    uint32_t refresh_mhz;
    uint32_t reserved;
    char     name[PLH_NAME_MAX]; /* "Virtual-plh-virtual" */
};
PLH_STATIC_ASSERT(sizeof(struct plh_vout_create_reply) == 96, "vout_create_reply");

struct plh_vout_ref {
    uint32_t output_id;
    uint32_t reserved;
};

#define PLH_DS_OFFER   0x1  /* PLH_VIRTUAL_DISPLAY=1 */
#define PLH_DS_AUTO    0x2  /* PLH_VIRTUAL_DISPLAY_AUTO=1 */
#define PLH_DS_FAKE_OFF 0x4 /* real outputs reported off by PLH_FAKE_SCREENS_OFF=1 (tests) */

struct plh_display_state {
    uint32_t flags;              /* PLH_DS_* */
    uint32_t mode_width, mode_height, mode_refresh_mhz; /* PLH_VIRTUAL_DISPLAY_MODE (width capped) */
    uint32_t real_total;         /* real (non-virtual) outputs known to KWin */
    uint32_t real_on;            /* of which DPMS on (or fake) */
    uint32_t real_off;           /* of which DPMS off / standby / suspend */
    uint32_t virtual_active;     /* 1 if the virtual output exists now */
    uint32_t virtual_output_id;  /* 0 if none */
    int32_t  vx, vy;             /* KWin rect of the virtual output (valid if virtual_active) */
    uint32_t vwidth, vheight;
    int32_t  real_right;         /* max x+width of the real outputs, KWin space (placeholder is placed here) */
    uint32_t reserved[6];
};
PLH_STATIC_ASSERT(sizeof(struct plh_display_state) == 80, "display_state");

/* ---- input redirect (service 0x03) ---- */
struct plh_redirect {
    struct plh_rect from;        /* Windows rect (Parsec's output rect, Windows coordinates) */
    struct plh_rect to;          /* KWin rect */
};
struct plh_input_redirect_hdr {
    uint32_t count;
    uint32_t reserved;
    /* struct plh_redirect r[count]; */
};

#endif
