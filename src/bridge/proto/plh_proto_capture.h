/*
 * plh_proto_capture.h — capture service (0x01xx). Owner: capture.
 * Included by plh_proto.h; do not include on its own.
 *
 * Flow:
 *   C: CAP_LIST_OUTPUTS            -> D: plh_cap_output_list (outputs of the real desktop, compositor
 *                                        logical coordinates, e.g. KWin)
 *   C: CAP_OPEN {output_id,...}    -> D: plh_cap_open_reply (shm name, geometry, slots)
 *                                     (status PLH_E_AGAIN: human approval of the portal pending,
 *                                      retry; a CAP_STATE STREAMING event will follow)
 *   D: CAP_FRAME (event)              the slot belongs to the client until CAP_RELEASE
 *   C: CAP_RELEASE {stream,slot}      (no reply)
 *   D: CAP_CURSOR (event)             position and, if shape_serial changed, the new shape
 *   C: CAP_CLOSE {stream}          -> D: empty reply
 *
 * Flow control: the daemon never writes into a slot held by the client. If all slots
 * are held, frames are dropped (dropped counter). The client must give back the old slot
 * as soon as it receives a newer one it does not intend to use.
 *
 * Shared memory of a stream (/dev/shm/<shm_name>):
 *   [0, 4096)                          struct plh_cap_shm_header
 *   [slot_offset + i*slot_size, ...)   slot i: pixels, stride bytes per row
 *   [cursor_offset + k*cursor_size)    k = shape_serial & 1: cursor shape (BGRA8 non-premultiplied,
 *                                      at most cursor_max x cursor_max, stride = width*4)
 */
#ifndef PLH_PROTO_CAPTURE_H
#define PLH_PROTO_CAPTURE_H

enum plh_msg_type_capture {
    PLH_MSG_CAP_LIST_OUTPUTS = 0x0101, /* C->D empty; reply plh_cap_output_list */
    PLH_MSG_CAP_OPEN         = 0x0102, /* C->D plh_cap_open; reply plh_cap_open_reply */
    PLH_MSG_CAP_CLOSE        = 0x0103, /* C->D plh_cap_stream_ref; empty reply */
    PLH_MSG_CAP_FRAME        = 0x0104, /* D->C event plh_cap_frame */
    PLH_MSG_CAP_RELEASE      = 0x0105, /* C->D plh_cap_release; no reply */
    PLH_MSG_CAP_CURSOR       = 0x0106, /* D->C event plh_cap_cursor */
    PLH_MSG_CAP_STATE        = 0x0107, /* D->C event plh_cap_state */
    PLH_MSG_CAP_STATS        = 0x0108, /* C->D plh_cap_stream_ref; reply plh_cap_stats */
    /* virtual display (see plh_proto_vdisplay.h) */
    PLH_MSG_VOUT_CREATE      = 0x0110, /* C->D plh_vout_create; reply plh_vout_create_reply */
    PLH_MSG_VOUT_DESTROY     = 0x0111, /* C->D plh_vout_ref; empty reply */
    PLH_MSG_DISPLAY_STATE    = 0x0112, /* C->D empty; reply plh_display_state. Also sent as an event on change */
};

enum plh_cap_backend {
    PLH_CAP_BACKEND_AUTO   = 0, /* chosen from the session (KWin direct, portal, X11) */
    PLH_CAP_BACKEND_PORTAL = 1, /* xdg-desktop-portal ScreenCast + PipeWire */
    PLH_CAP_BACKEND_KWIN   = 2, /* zkde_screencast_unstable_v1 + PipeWire */
    PLH_CAP_BACKEND_TEST   = 3, /* synthetic test pattern (tests without a desktop) */
    PLH_CAP_BACKEND_X11    = 4, /* X11: XShm (RandR for outputs) */
};

#define PLH_CAP_OUTPUT_PRIMARY 0x1
#define PLH_CAP_OUTPUT_VIRTUAL 0x2 /* output created by VOUT_CREATE (KWin "plh-virtual") */

struct plh_cap_output {
    uint32_t output_id;          /* stable during the daemon lifetime */
    int32_t  x, y;               /* position in the virtual desktop (logical pixels) */
    uint32_t width, height;      /* size in pixels of the current mode */
    uint32_t refresh_mhz;        /* 60000 = 60 Hz */
    uint32_t scale_x1000;        /* 1000 = 100 % */
    uint32_t rotation;           /* 0, 90, 180, 270 */
    uint32_t flags;              /* PLH_CAP_OUTPUT_* */
    uint32_t reserved;
    char     name[PLH_NAME_MAX];        /* connector: "DP-2" */
    char     description[PLH_NAME_MAX]; /* make/model */
};
PLH_STATIC_ASSERT(sizeof(struct plh_cap_output) == 168, "cap_output");

struct plh_cap_output_list {
    uint32_t count;
    uint32_t reserved;
    /* struct plh_cap_output outputs[count]; */
};

#define PLH_CAP_F_CURSOR_METADATA 0x1 /* cursor separate (CAP_CURSOR), not drawn into the image */
#define PLH_CAP_F_CURSOR_EMBEDDED 0x2 /* cursor drawn into the pixels */

struct plh_cap_open {
    uint32_t output_id;
    uint32_t backend;            /* enum plh_cap_backend */
    uint32_t flags;              /* PLH_CAP_F_* */
    uint32_t max_fps;            /* 0 = display refresh rate */
    uint32_t slot_count;         /* 0 = default (3), max PLH_CAP_MAX_SLOTS */
    uint32_t format;             /* requested format (enum plh_pixel_format), 0 = BGRA8 */
};

#define PLH_CAP_MAX_SLOTS 8

struct plh_cap_open_reply {
    int32_t  status;
    uint32_t stream_id;
    uint32_t width, height, stride, format;
    uint32_t slot_count;
    uint32_t slot_size;          /* multiple of 4096 */
    uint64_t slot_offset;
    uint64_t cursor_offset;
    uint32_t cursor_size;        /* size of one cursor buffer */
    uint32_t cursor_max;         /* max cursor width/height (256) */
    uint64_t shm_size;
    uint32_t backend;            /* backend actually used */
    uint32_t reserved;
    char     shm_name[PLH_NAME_MAX]; /* name in /dev/shm, e.g. "plh-<uid>-cap-3" */
};
PLH_STATIC_ASSERT(sizeof(struct plh_cap_open_reply) == 136, "cap_open_reply");

struct plh_cap_stream_ref {
    uint32_t stream_id;
    uint32_t reserved;
};

struct plh_rect {
    int32_t left, top, right, bottom;  /* like the Windows RECT */
};

#define PLH_CAP_FRAME_F_DMABUF_BACKED 0x1 /* a native DMA-BUF is attached (zero-copy possible) */

struct plh_cap_frame {
    uint32_t stream_id;
    uint32_t slot;
    uint64_t frame_id;           /* PLH_FRAME_ID(stream, n) */
    uint64_t pts_ns;             /* capture time, daemon CLOCK_MONOTONIC */
    uint64_t ready_ns;           /* time when the pixels are in the slot */
    uint32_t width, height, stride, format; /* may change (resize) */
    uint32_t flags;              /* PLH_CAP_FRAME_F_* */
    uint32_t dropped;            /* frames dropped since the previous one */
    uint32_t dirty_count;        /* 0 = whole image */
    uint32_t reserved;
    /* struct plh_rect dirty[dirty_count]; */
};
PLH_STATIC_ASSERT(sizeof(struct plh_cap_frame) == 64, "cap_frame");

struct plh_cap_release {
    uint32_t stream_id;
    uint32_t slot;
};

struct plh_cap_cursor {
    uint32_t stream_id;
    uint32_t visible;
    int32_t  x, y;               /* hotspot position, relative to the output */
    uint64_t pts_ns;
    uint32_t shape_serial;       /* changes when the shape changes; buffer = serial & 1 */
    uint32_t shape_changed;      /* 1 if the shape below is new */
    uint32_t width, height;
    int32_t  hot_x, hot_y;
};
PLH_STATIC_ASSERT(sizeof(struct plh_cap_cursor) == 48, "cap_cursor");

enum plh_cap_stream_state {
    PLH_CAP_STATE_WAITING_APPROVAL = 1, /* portal dialog shown to the human */
    PLH_CAP_STATE_STREAMING = 2,
    PLH_CAP_STATE_PAUSED = 3,
    PLH_CAP_STATE_ERROR = 4,
    PLH_CAP_STATE_CLOSED = 5,
};

struct plh_cap_state {
    uint32_t stream_id;
    uint32_t state;
    int32_t  status;
    uint32_t reserved;
    char     message[128];
};

struct plh_cap_stats {
    uint32_t stream_id;
    uint32_t reserved;
    uint64_t frames_captured;
    uint64_t frames_sent;
    uint64_t frames_dropped;
    uint64_t last_capture_to_ready_ns; /* copy/convert of the last frame */
};

#define PLH_CAP_SHM_MAGIC 0x50414348u /* "HCAP" */
struct plh_cap_shm_header {
    uint32_t magic;
    uint32_t version;            /* 1 */
    uint32_t width, height, stride, format;
    uint32_t slot_count, slot_size;
    uint64_t slot_offset, cursor_offset;
    uint32_t cursor_size, cursor_max;
};

#include "plh_proto_vdisplay.h"

#endif /* PLH_PROTO_CAPTURE_H */
