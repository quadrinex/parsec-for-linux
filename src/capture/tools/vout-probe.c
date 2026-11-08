/* vout-probe.c — spike S2: ask KWin for a virtual output (zkde_screencast stream_virtual_output[_with_description]),
 * count the frames of its PipeWire stream for a few seconds, then close the stream.
 * Owner: capture. Observation only; not part of the bridge.
 * usage: vout-probe [-w W] [-h H] [-t SECONDS] [-n NAME] [-d] [-k]
 *   -d  use stream_virtual_output (no description) instead of ..._with_description
 *   -k  do not close the stream at the end: sleep until killed (SIGKILL experiment)
 * Prints "READY" on stdout once frames can flow, so a script can take snapshots. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/pod/builder.h>
#include "zkde-screencast-unstable-v1-client-protocol.h"

static struct wl_display *dpy;
static struct zkde_screencast_unstable_v1 *sc;
static uint32_t sc_version;
static struct pw_main_loop *loop;
static struct pw_context *ctx;
static struct pw_core *core;
static struct pw_stream *pws;
static struct spa_hook pws_l;
static int w = 1920, h = 1080, secs = 5, use_desc = 1, keep = 0;
static unsigned long frames, first_ms, last_ms;
static unsigned long long serial;
static uint32_t node;
static int failed, closed_by_kwin;
static struct spa_video_info_raw vinfo;

static unsigned long now_ms(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000UL + t.tv_nsec / 1000000UL;
}

static void on_state(void *d, enum pw_stream_state o, enum pw_stream_state s, const char *e)
{ (void)d; (void)o; fprintf(stderr, "pw state: %s%s%s\n", pw_stream_state_as_string(s), e ? ": " : "", e ? e : ""); }
static void on_param(void *d, uint32_t id, const struct spa_pod *p)
{
    (void)d;
    if (id != SPA_PARAM_Format || !p) return;
    spa_format_video_raw_parse(p, &vinfo);
    fprintf(stderr, "pw format: %ux%u fmt=%u framerate=%u/%u max=%u/%u\n", vinfo.size.width, vinfo.size.height, vinfo.format,
            vinfo.framerate.num, vinfo.framerate.denom, vinfo.max_framerate.num, vinfo.max_framerate.denom);
    uint8_t buf[256]; struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof buf);
    const struct spa_pod *ps[1];
    ps[0] = spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
        SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(4, 2, 16), SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
        SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_MemFd) | (1 << SPA_DATA_MemPtr)));
    pw_stream_update_params(pws, ps, 1);
}
static void on_process(void *d)
{
    (void)d;
    struct pw_buffer *pb; struct pw_buffer *last = NULL;
    while ((pb = pw_stream_dequeue_buffer(pws))) {
        if (last) pw_stream_queue_buffer(pws, last);
        last = pb;
        frames++;
        last_ms = now_ms();
        if (!first_ms) { first_ms = last_ms; printf("READY\n"); fflush(stdout); }
    }
    if (last) pw_stream_queue_buffer(pws, last);
}
static const struct pw_stream_events pwe = { PW_VERSION_STREAM_EVENTS, .state_changed = on_state, .param_changed = on_param, .process = on_process };

static void start_pw(const char *target)
{
    struct pw_properties *pr = pw_properties_new(PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_ROLE, "Screen", NULL);
    pw_properties_set(pr, PW_KEY_TARGET_OBJECT, target);
    pws = pw_stream_new(core, "plh-vout-probe", pr);
    pw_stream_add_listener(pws, &pws_l, &pwe, NULL);
    uint8_t buf[1024]; struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof buf);
    const struct spa_pod *ps[1];
    ps[0] = spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
        SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video), SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
        SPA_FORMAT_VIDEO_format, SPA_POD_CHOICE_ENUM_Id(5, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_RGBx, SPA_VIDEO_FORMAT_RGBA),
        SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(&SPA_RECTANGLE(w, h), &SPA_RECTANGLE(1, 1), &SPA_RECTANGLE(8192, 8192)),
        SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&SPA_FRACTION(0, 1)),
        SPA_FORMAT_VIDEO_maxFramerate, SPA_POD_CHOICE_RANGE_Fraction(&SPA_FRACTION(60, 1), &SPA_FRACTION(1, 1), &SPA_FRACTION(60, 1)));
    int r = pw_stream_connect(pws, PW_DIRECTION_INPUT, PW_ID_ANY, PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS, ps, 1);
    fprintf(stderr, "pw_stream_connect target=%s -> %d\n", target, r);
}

static void st_closed(void *d, struct zkde_screencast_stream_unstable_v1 *st)
{ (void)d; (void)st; fprintf(stderr, "wl: stream closed by KWin\n"); closed_by_kwin = 1; pw_main_loop_quit(loop); }
static void st_created(void *d, struct zkde_screencast_stream_unstable_v1 *st, uint32_t n)
{
    (void)d; (void)st; node = n; fprintf(stderr, "wl: created node=%u\n", n);
    if (sc_version < 6) { char t[16]; snprintf(t, sizeof t, "%u", n); start_pw(t); }
}
static void st_failed(void *d, struct zkde_screencast_stream_unstable_v1 *st, const char *e)
{ (void)d; (void)st; fprintf(stderr, "wl: FAILED: %s\n", e); failed = 1; pw_main_loop_quit(loop); }
static void st_serial(void *d, struct zkde_screencast_stream_unstable_v1 *st, uint32_t hi, uint32_t lo)
{
    (void)d; (void)st; serial = ((unsigned long long)hi << 32) | lo;
    fprintf(stderr, "wl: object.serial=%llu\n", serial);
    char t[32]; snprintf(t, sizeof t, "%llu", serial); start_pw(t);
}
static const struct zkde_screencast_stream_unstable_v1_listener st_l = { st_closed, st_created, st_failed, st_serial };

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver)
{
    (void)d;
    if (!strcmp(iface, zkde_screencast_unstable_v1_interface.name)) {
        sc_version = ver < 6 ? ver : 6;
        sc = wl_registry_bind(r, name, &zkde_screencast_unstable_v1_interface, sc_version);
    }
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }
static const struct wl_registry_listener reg_l = { reg_global, reg_remove };

static void on_wl(void *d, int fd, uint32_t mask)
{
    (void)d; (void)fd; (void)mask;
    if (wl_display_dispatch(dpy) < 0) { fprintf(stderr, "wl dispatch error\n"); pw_main_loop_quit(loop); }
    wl_display_flush(dpy);
}
static void on_timeout(void *d, uint64_t e) { (void)d; (void)e; pw_main_loop_quit(loop); }

int main(int argc, char **argv)
{
    const char *name = "plh-virtual";
    int o;
    while ((o = getopt(argc, argv, "w:h:t:n:dk")) != -1) {
        switch (o) {
        case 'w': w = atoi(optarg); break;
        case 'h': h = atoi(optarg); break;
        case 't': secs = atoi(optarg); break;
        case 'n': name = optarg; break;
        case 'd': use_desc = 0; break;
        case 'k': keep = 1; break;
        default: return 2;
        }
    }
    pw_init(&argc, &argv);
    dpy = wl_display_connect(NULL);
    if (!dpy) { fprintf(stderr, "no wayland\n"); return 1; }
    struct wl_registry *reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &reg_l, NULL);
    wl_display_roundtrip(dpy);
    if (!sc) { fprintf(stderr, "zkde_screencast not advertised\n"); return 1; }
    fprintf(stderr, "zkde_screencast v%u\n", sc_version);

    loop = pw_main_loop_new(NULL);
    ctx = pw_context_new(pw_main_loop_get_loop(loop), NULL, 0);
    core = pw_context_connect(ctx, NULL, 0);
    if (!core) { fprintf(stderr, "no pipewire\n"); return 1; }
    struct pw_loop *lp = pw_main_loop_get_loop(loop);
    pw_loop_add_io(lp, wl_display_get_fd(dpy), SPA_IO_IN, false, on_wl, NULL);

    struct zkde_screencast_stream_unstable_v1 *st;
    if (use_desc && sc_version >= 4)
        st = zkde_screencast_unstable_v1_stream_virtual_output_with_description(sc, name, "Virtual display (parsec-for-linux)", w, h,
                wl_fixed_from_int(1), ZKDE_SCREENCAST_UNSTABLE_V1_POINTER_METADATA);
    else
        st = zkde_screencast_unstable_v1_stream_virtual_output(sc, name, w, h, wl_fixed_from_int(1), ZKDE_SCREENCAST_UNSTABLE_V1_POINTER_METADATA);
    zkde_screencast_stream_unstable_v1_add_listener(st, &st_l, NULL);
    wl_display_flush(dpy);
    fprintf(stderr, "requested %s %dx%d (%s)\n", name, w, h, use_desc ? "with_description" : "plain");

    if (!keep) {
        struct spa_source *t = pw_loop_add_timer(lp, on_timeout, NULL);
        struct timespec ts = { secs, 0 };
        pw_loop_update_timer(lp, t, &ts, NULL, false);
    }
    pw_main_loop_run(loop);

    unsigned long dur = first_ms ? last_ms - first_ms : 0;
    printf("RESULT frames=%lu duration_ms=%lu fps=%.1f failed=%d closed_by_kwin=%d fmt=%ux%u\n", frames, dur,
           dur ? frames * 1000.0 / dur : 0.0, failed, closed_by_kwin, vinfo.size.width, vinfo.size.height);
    if (pws) pw_stream_destroy(pws);
    if (!closed_by_kwin && !failed) zkde_screencast_stream_unstable_v1_close(st);
    wl_display_roundtrip(dpy);
    printf("CLOSED\n"); fflush(stdout);
    pw_core_disconnect(core);
    return failed;
}
