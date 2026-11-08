/* cap_internal.h — internal to the daemon capture service. Owner: capture. */
#ifndef CAP_INTERNAL_H
#define CAP_INTERNAL_H

#include "../bridge/plh_bridge.h"

#define CAP_MAX_OUTPUTS 16

/* outputs of the desktop (outputs_wl.c); returns the count, <0 if no Wayland */
int cap_outputs_query(struct plh_cap_output *out, int max);
int cap_outputs_query_wl(struct plh_cap_output *out, int max);
int cap_outputs_query_x11(struct plh_cap_output *out, int max);
uint32_t cap_stable_id(const char *name);

struct cap_stream;

/* virtual output (backend_kwin.c anchor stream, vout.c logic) */
int  cap_kwin_vout_start(const char *name, const char *desc, uint32_t w, uint32_t h, void (*lost)(void));
void cap_kwin_vout_stop(void);
int  cap_kwin_vout_alive(void);
void cap_vout_handle(struct plh_client *c, const struct plh_msg_hdr *h, const void *p);
void cap_vout_client_gone(struct plh_client *c);
void cap_vout_fini(void);
#define CAP_VOUT_CONNECTOR "Virtual-" PLH_VOUT_NAME

struct cap_backend_ops {
    const char *name;
    uint32_t    id; /* enum plh_cap_backend */
    /* starts capturing `out`; 0 = OK (start may be asynchronous),
     * PLH_E_AGAIN = waiting for approval */
    int  (*start)(struct cap_stream *s, const struct plh_cap_output *out);
    void (*stop)(struct cap_stream *s);
};

extern const struct cap_backend_ops cap_backend_test;
extern const struct cap_backend_ops cap_backend_portal;
extern const struct cap_backend_ops cap_backend_kwin;
extern const struct cap_backend_ops cap_backend_x11;
/* order in which AUTO tries backends, from the session (NULL-terminated); returns the count */
int cap_token_load(const char *out, char *tok, size_t n);
int cap_token_save(const char *out, const char *tok);
int cap_detect_order(const struct cap_backend_ops **order, int max);

struct cap_stream {
    uint32_t            id;
    struct plh_client  *client;
    struct plh_cap_output out;
    const struct cap_backend_ops *be;
    void               *be_data;
    uint32_t            flags;     /* PLH_CAP_F_* requested */
    uint32_t            max_fps;
    uint32_t            format;
    struct plh_shm      shm;
    struct plh_cap_shm_header *hdr;
    uint32_t            width, height, stride;
    uint32_t            slot_count, slot_size;
    uint64_t            slot_offset, cursor_offset;
    uint32_t            cursor_size, cursor_max;
    uint8_t             slot_held[PLH_CAP_MAX_SLOTS]; /* 1 = held by the client */
    int                 writing;   /* slot being written or -1 */
    uint32_t            next_slot;
    uint64_t            frame_no;
    uint64_t            captured, sent, dropped, dropped_since;
    uint64_t            last_copy_ns;
    uint64_t            last_sent_ns;
    uint32_t            cursor_serial;
    struct plh_cap_cursor last_cursor;
    struct cap_stream  *next;
};

/* API for backends (loop thread only) */
/* Returns a pointer to a free slot (stride = s->stride) or NULL (client saturated: drop the frame) */
uint8_t *cap_stream_begin_frame(struct cap_stream *s, uint32_t *slot_out);
void     cap_stream_commit_frame(struct cap_stream *s, uint32_t slot, uint64_t pts_ns, uint32_t flags);
void     cap_stream_drop_frame(struct cap_stream *s);
/* Cursor: BGRA8 pixels (NULL if unchanged). x,y relative to the output (hotspot). */
void     cap_stream_cursor(struct cap_stream *s, int visible, int32_t x, int32_t y,
                           const uint8_t *bgra, uint32_t w, uint32_t h, uint32_t src_stride,
                           int32_t hot_x, int32_t hot_y);
void     cap_stream_state(struct cap_stream *s, uint32_t state, int32_t status, const char *msg);

#endif
