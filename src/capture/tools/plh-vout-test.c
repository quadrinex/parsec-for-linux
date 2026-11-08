/* plh-vout-test.c — native/PE test client for the virtual display messages. Owner: capture.
 * usage: plh-vout-test state
 *        plh-vout-test create [-w W -h H] [-t HOLD_S] [-f] [-d]
 *   create: VOUT_CREATE, print the rect, list outputs, hold HOLD_S seconds (default 2; -k = until killed),
 *   -f: also CAP_OPEN the virtual output and count frames while holding, -d: VOUT_DESTROY before exit
 *   (without -d the connection is simply closed: the bridge must remove the output).
 * Prints machine-readable lines: STATE ..., VOUT ..., LIST ..., FRAMES n, DESTROYED. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../../bridge/client/plh_client.h"

static unsigned frames;
static void on_event(void *u, const struct plh_msg_hdr *h, const void *p)
{
    struct plh_conn *c = u;
    if (h->type == PLH_MSG_CAP_FRAME) {
        const struct plh_cap_frame *f = p;
        frames++;
        struct plh_cap_release r = { f->stream_id, f->slot };
        plh_conn_send(c, PLH_MSG_CAP_RELEASE, &r, sizeof r);
    }
}

static int get_state(struct plh_conn *c)
{
    struct plh_display_state s; uint32_t n = 0;
    int r = plh_conn_request(c, PLH_MSG_DISPLAY_STATE, NULL, 0, &s, sizeof s, &n, 5000);
    if (r) { printf("STATE error %d\n", r); return r; }
    printf("STATE offer=%d auto=%d fake_off=%d mode=%ux%u@%u real_total=%u real_on=%u real_off=%u virtual=%u id=%u rect=%d,%d,%ux%u real_right=%d\n",
           !!(s.flags & PLH_DS_OFFER), !!(s.flags & PLH_DS_AUTO), !!(s.flags & PLH_DS_FAKE_OFF), s.mode_width, s.mode_height,
           s.mode_refresh_mhz / 1000, s.real_total, s.real_on, s.real_off, s.virtual_active, s.virtual_output_id, s.vx, s.vy,
           s.vwidth, s.vheight, s.real_right);
    return 0;
}

static unsigned list(struct plh_conn *c, unsigned *vid)
{
    uint8_t buf[sizeof(struct plh_cap_output_list) + 16 * sizeof(struct plh_cap_output)]; uint32_t n = 0;
    *vid = 0;
    if (plh_conn_request(c, PLH_MSG_CAP_LIST_OUTPUTS, NULL, 0, buf, sizeof buf, &n, 5000)) return 0;
    const struct plh_cap_output_list *l = (void *)buf;
    const struct plh_cap_output *o = (void *)(buf + sizeof *l);
    for (unsigned i = 0; i < l->count; i++) {
        printf("LIST %s id=%u %d,%d %ux%u flags=0x%x\n", o[i].name, o[i].output_id, o[i].x, o[i].y, o[i].width, o[i].height, o[i].flags);
        if (o[i].flags & PLH_CAP_OUTPUT_VIRTUAL) *vid = o[i].output_id;
    }
    return l->count;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s state|create [-w W -h H -t S -k -f -d]\n", argv[0]); return 2; }
    unsigned w = 0, h = 0, hold = 2; int keep = 0, frm = 0, destroy = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "-w") && i + 1 < argc) w = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "-h") && i + 1 < argc) h = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "-t") && i + 1 < argc) hold = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "-k")) keep = 1;
        else if (!strcmp(argv[i], "-f")) frm = 1;
        else if (!strcmp(argv[i], "-d")) destroy = 1;
    }
    struct plh_conn *c = NULL;
    int r = plh_conn_open(&c, PLH_CLIENT_TEST, "plh-vout-test", NULL);
    if (r) { fprintf(stderr, "cannot connect to the bridge (%d)\n", r); return 1; }
    plh_conn_set_event_cb(c, on_event, c);
    if (!strcmp(argv[1], "state")) { r = get_state(c); plh_conn_close(c); return r != 0; }
    if (strcmp(argv[1], "create")) return 2;

    struct plh_vout_create rq = { w, h, 0, 0 };
    struct plh_vout_create_reply rep; uint32_t n = 0;
    r = plh_conn_request(c, PLH_MSG_VOUT_CREATE, &rq, sizeof rq, &rep, sizeof rep, &n, 10000);
    if (r) { printf("VOUT error %d\n", r); plh_conn_close(c); return 1; }
    printf("VOUT id=%u rect=%d,%d,%ux%u name=%s\n", rep.output_id, rep.x, rep.y, rep.width, rep.height, rep.name);
    unsigned vid; list(c, &vid);
    get_state(c);
    fflush(stdout);
    uint32_t stream = 0;
    if (frm) {
        struct plh_cap_open op = { rep.output_id, PLH_CAP_BACKEND_AUTO, PLH_CAP_F_CURSOR_METADATA, 0, 4, PLH_FMT_BGRA8 };
        struct plh_cap_open_reply orp; uint32_t rl = 0;
        r = plh_conn_request(c, PLH_MSG_CAP_OPEN, &op, sizeof op, &orp, sizeof orp, &rl, 20000);
        if (r) printf("OPEN error %d\n", r); else { stream = orp.stream_id; printf("OPEN stream=%u %ux%u\n", stream, orp.width, orp.height); }
        fflush(stdout);
    }
    printf("READY\n"); fflush(stdout);
    for (unsigned s = 0; keep || s < hold * 10; s++) { plh_conn_poll(c, 100); }
    if (frm) printf("FRAMES %u\n", frames);
    if (stream) { struct plh_cap_stream_ref sr = { stream, 0 }; plh_conn_request(c, PLH_MSG_CAP_CLOSE, &sr, sizeof sr, NULL, 0, NULL, 3000); }
    if (destroy) {
        struct plh_vout_ref ref = { rep.output_id, 0 };
        r = plh_conn_request(c, PLH_MSG_VOUT_DESTROY, &ref, sizeof ref, NULL, 0, NULL, 5000);
        printf("DESTROYED %d\n", r);
    }
    fflush(stdout);
    plh_conn_close(c);
    return 0;
}
