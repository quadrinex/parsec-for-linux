/* plh-input-ctl — injects input through a plh_input backend (test/diagnostics).
 * Commands (arguments or stdin, one per line):
 *   abs X Y | rel DX DY | key NAME down|up|tap | sc SCAN [e0] down|up | vk VK [ext] down|up
 *   btn left|right|middle|x1|x2 down|up|click | wheel V120 H120 | sleep MS
 *   resize X Y W H   recreate the absolute device for a new desktop rect; prints the recreation time
 *                    and the time until KWin has discovered the new device (the input "gap")
 * Options: --desktop X,Y,WxH (default 0,0,5760x1080), --settle MS (default 800),
 *          --no-flat (do not set the KWin flat acceleration profile on the relative mouse). */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "plh_input.h"

static void msleep(int ms) { struct timespec t = { ms / 1000, (ms % 1000) * 1000000L }; nanosleep(&t, NULL); }

static int updown(const char *s) { return s && (!strcmp(s, "down") || !strcmp(s, "1")); }

static int run(plh_input *in, char *line)
{
    char *v[7] = {0}; int n = 0;
    for (char *t = strtok(line, " \t\r\n"); t && n < 7; t = strtok(NULL, " \t\r\n")) v[n++] = t;
    if (!n || v[0][0] == '#') return 0;
    int r = -1;
    if (!strcmp(v[0], "abs") && n == 3) r = plh_input_move_abs(in, atoi(v[1]), atoi(v[2]));
    else if (!strcmp(v[0], "rel") && n == 3) r = plh_input_move_rel(in, atoi(v[1]), atoi(v[2]));
    else if (!strcmp(v[0], "wheel") && n == 3) r = plh_input_wheel(in, atoi(v[1]), atoi(v[2]));
    else if (!strcmp(v[0], "resize") && n == 5) {
        plh_rect nr = { atoi(v[1]), atoi(v[2]), atoi(v[3]), atoi(v[4]) };
        struct timespec t0, t1; char k[16], rl[16], a[16], err[128];
        clock_gettime(CLOCK_MONOTONIC, &t0);
        r = plh_uinput_resize(in, &nr);
        if (r == 0 && !plh_uinput_evnodes(in, k, rl, a)) {
            /* wait until KWin exposes the new absolute node on DBus: that is when it can deliver events */
            int d = plh_kwin_set_flat_accel(a, 3000, err, sizeof err);
            clock_gettime(CLOCK_MONOTONIC, &t1);
            printf("RESIZE %s create=%ld us kwin-discovered=%s after %ld ms\n", a, plh_uinput_last_resize_us(in), d ? "NO" : "yes",
                   (t1.tv_sec - t0.tv_sec) * 1000L + (t1.tv_nsec - t0.tv_nsec) / 1000000L);
            fflush(stdout);
        } else printf("RESIZE result %d\n", r);
    }
    else if (!strcmp(v[0], "sleep") && n == 2) { msleep(atoi(v[1])); r = 0; }
    else if (!strcmp(v[0], "key") && n == 3) {
        int c = plh_evdev_from_name(v[1]);
        if (!c) { fprintf(stderr, "unknown key %s\n", v[1]); return -1; }
        if (!strcmp(v[2], "tap")) { r = plh_input_key_evdev(in, c, 1); msleep(20); if (!r) r = plh_input_key_evdev(in, c, 0); }
        else r = plh_input_key_evdev(in, c, updown(v[2]));
    } else if ((!strcmp(v[0], "sc") || !strcmp(v[0], "vk")) && (n == 3 || n == 4)) {
        unsigned code = (unsigned)strtoul(v[1], NULL, 0);
        int ext = n == 4;
        int down = updown(v[n - 1]);
        r = v[0][0] == 's' ? plh_input_key_scancode(in, code, ext, down) : plh_input_key_vk(in, code, ext, down);
    } else if (!strcmp(v[0], "btn") && n == 3) {
        static const char *bn[] = { "left", "right", "middle", "x1", "x2" };
        int b = -1;
        for (int i = 0; i < 5; i++) if (!strcmp(v[1], bn[i])) b = i;
        if (b < 0) return -1;
        if (!strcmp(v[2], "click")) { r = plh_input_button(in, b, 1); msleep(20); if (!r) r = plh_input_button(in, b, 0); }
        else r = plh_input_button(in, b, updown(v[2]));
    }
    if (r) fprintf(stderr, "failed: %s (%d)\n", v[0], r);
    return r;
}

int main(int argc, char **argv)
{
    plh_rect d = { 0, 0, 5760, 1080 };
    int settle = 800, i = 1, rc = 0, flat = 1;
    for (; i < argc && !strncmp(argv[i], "--", 2); i++) {
        if (!strcmp(argv[i], "--desktop") && i + 1 < argc) {
            if (sscanf(argv[++i], "%d,%d,%dx%d", &d.x, &d.y, &d.w, &d.h) != 4) { fprintf(stderr, "--desktop X,Y,WxH\n"); return 2; }
        } else if (!strcmp(argv[i], "--settle") && i + 1 < argc) settle = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--no-flat")) flat = 0;
        else { fprintf(stderr, "option inconnue %s\n", argv[i]); return 2; }
    }
    char err[256];
    plh_input *in = plh_uinput_open(&d, err, sizeof err);
    if (!in) { fprintf(stderr, "plh_uinput_open: %s\n", err); return 1; }
    if (flat) {
        char k[16], r[16], a[16];
        if (plh_uinput_evnodes(in, k, r, a) || plh_kwin_set_flat_accel(r, 3000, err, sizeof err))
            fprintf(stderr, "warning: flat acceleration not set: %s\n", err);
    }
    msleep(settle);   /* let udev/libinput/KWin discover the devices */
    if (i < argc) {
        /* arguments: commands separated by "," */
        char buf[4096] = "";
        for (; i < argc; i++) { strncat(buf, argv[i], sizeof buf - strlen(buf) - 2); strcat(buf, " "); }
        for (char *s = buf, *e; s; s = e) {
            e = strchr(s, ','); if (e) *e++ = 0;
            char tmp[512]; snprintf(tmp, sizeof tmp, "%s", s);
            if (run(in, tmp)) rc = 1;
        }
    } else {
        char line[512];
        while (fgets(line, sizeof line, stdin)) if (run(in, line)) rc = 1;
    }
    msleep(100);
    plh_input_close(in);
    return rc;
}
