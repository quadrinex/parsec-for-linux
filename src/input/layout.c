/* layout.c — KWin screen layout and Windows -> KWin translation.
 * Windows (hence Wine/Parsec) puts the primary screen at 0,0; KWin has its own origin.
 * Find the translation T such that each Windows monitor + T = a KWin output of the same size. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plh_input.h"

int plh_kwin_outputs(struct plh_output *o, int max)
{
    FILE *f = popen("kscreen-doctor -o 2>/dev/null", "r");
    if (!f) return -1;
    char line[512];
    int n = -1, enabled = 0;
    while (fgets(line, sizeof line, f)) {
        /* strip ANSI colour sequences */
        char clean[512]; size_t j = 0;
        for (size_t i = 0; line[i] && j + 1 < sizeof clean; i++) {
            if (line[i] == 0x1b) { while (line[i] && line[i] != 'm') i++; if (!line[i]) break; continue; }
            clean[j++] = line[i];
        }
        clean[j] = 0;
        char *s = clean; while (*s == ' ' || *s == '\t') s++;
        if (!strncmp(s, "Output:", 7)) {
            if (n >= 0 && enabled && o[n].w > 0) n++;
            if (n < 0) n = 0;
            if (n >= max) break;
            memset(&o[n], 0, sizeof o[n]);
            enabled = 0;
            sscanf(s, "Output: %*d %31s", o[n].name);
        } else if (n >= 0 && n < max) {
            if (!strncmp(s, "enabled", 7)) enabled = 1;
            else if (!strncmp(s, "priority ", 9)) o[n].priority = atoi(s + 9);
            else if (!strncmp(s, "Geometry:", 9)) sscanf(s, "Geometry: %d,%d %dx%d", &o[n].x, &o[n].y, &o[n].w, &o[n].h);
        }
    }
    if (n >= 0 && n < max && enabled && o[n].w > 0) n++;
    pclose(f);
    return n < 0 ? 0 : n;
}

plh_rect plh_outputs_bounds(const struct plh_output *o, int n)
{
    plh_rect r = { 0, 0, 0, 0 };
    if (n <= 0) return r;
    int x0 = o[0].x, y0 = o[0].y, x1 = o[0].x + o[0].w, y1 = o[0].y + o[0].h;
    for (int i = 1; i < n; i++) {
        if (o[i].x < x0) x0 = o[i].x;
        if (o[i].y < y0) y0 = o[i].y;
        if (o[i].x + o[i].w > x1) x1 = o[i].x + o[i].w;
        if (o[i].y + o[i].h > y1) y1 = o[i].y + o[i].h;
    }
    r.x = x0; r.y = y0; r.w = x1 - x0; r.h = y1 - y0;
    return r;
}

int plh_output_is_virtual(const char *name)
{
    return name && strstr(name, "plh-virtual") != NULL;
}

int plh_layout_snapshot(const struct plh_output *k, int nk, plh_rect *desk, int *tx, int *ty)
{
    *desk = plh_outputs_bounds(k, nk);
    if (nk <= 0) return -1;
    if (plh_kwin_primary_translation(k, nk, tx, ty) == 0) return 0;
    for (int j = 0; j < nk; j++)
        if (!plh_output_is_virtual(k[j].name)) { *tx = k[j].x; *ty = k[j].y; return 0; }
    return -1;
}

int plh_redirect_map(const struct plh_redir *r, int n, int wx, int wy, int *kx, int *ky)
{
    for (int i = 0; i < n; i++) {
        const plh_rect *f = &r[i].from, *t = &r[i].to;
        if (f->w <= 0 || f->h <= 0 || t->w <= 0 || t->h <= 0) continue;
        if (wx < f->x || wy < f->y || wx >= f->x + f->w || wy >= f->y + f->h) continue;
        *kx = t->x + (int)(((long long)(wx - f->x) * t->w) / f->w);
        *ky = t->y + (int)(((long long)(wy - f->y) * t->h) / f->h);
        return 1;
    }
    return 0;
}

static int matches(const plh_rect *win, int nw, const struct plh_output *k, int nk, int tx, int ty)
{
    for (int i = 0; i < nw; i++) {
        int ok = 0;
        for (int j = 0; j < nk && !ok; j++)
            ok = win[i].x + tx == k[j].x && win[i].y + ty == k[j].y && win[i].w == k[j].w && win[i].h == k[j].h;
        if (!ok) return 0;
    }
    return 1;
}

int plh_layout_translation(const plh_rect *win, int nw, int win_primary,
                           const struct plh_output *k, int nk, int *tx, int *ty)
{
    int found = 0, best = 0;
    if (nw <= 0 || nk <= 0) return -1;
    int p = (win_primary >= 0 && win_primary < nw) ? win_primary : 0;
    for (int j = 0; j < nk; j++) {
        if (plh_output_is_virtual(k[j].name)) continue;   /* never an anchor for the Windows primary */
        int cx = k[j].x - win[p].x, cy = k[j].y - win[p].y;
        if (!matches(win, nw, k, nk, cx, cy)) continue;
        /* tie-break: prefer the primary KWin output (priority 1) for the Windows primary */
        int score = k[j].priority == 1 ? 2 : 1;
        if (score > best) { best = score; *tx = cx; *ty = cy; found = 1; }
    }
    return found ? 0 : -1;
}

int plh_kwin_primary_translation(const struct plh_output *k, int nk, int *tx, int *ty)
{
    for (int j = 0; j < nk; j++)
        if (k[j].priority == 1 && !plh_output_is_virtual(k[j].name)) { *tx = k[j].x; *ty = k[j].y; return 0; }
    return -1;
}

/* ---- Generic layout detection (non-KWin): pure text parsers + launchers ---- */

/* wlr-randr: "NAME \"desc\"", then "Enabled: yes", "1920x1080 px, 60 Hz (current)", "Position: x,y". */
int plh_parse_wlr_randr(const char *txt, struct plh_output *o, int max)
{
    int n = 0, en = 1;
    char *copy = strdup(txt), *save = NULL;
    if (!copy) return -1;
    for (char *l = strtok_r(copy, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) {
        if (*l != ' ' && *l != '\t') {
            if (n < max) {
                memset(&o[n], 0, sizeof o[n]); en = 1;
                sscanf(l, "%31s", o[n].name);
                n++;
            }
            continue;
        }
        if (!n) continue;
        struct plh_output *c = &o[n - 1];
        char *s = l; while (*s == ' ' || *s == '\t') s++;
        if (!strncmp(s, "Enabled:", 8)) en = !strstr(s, "no");
        else if (!strncmp(s, "Position:", 9)) sscanf(s, "Position: %d,%d", &c->x, &c->y);
        else if (strstr(s, "(current)")) sscanf(s, "%dx%d", &c->w, &c->h);
        if (!en) c->w = 0;
    }
    free(copy);
    int k = 0;                         /* drop disabled outputs / outputs without a mode */
    for (int i = 0; i < n; i++) if (o[i].w > 0) { if (k != i) o[k] = o[i]; k++; }
    for (int i = 0; i < k; i++) if (o[i].x == 0 && o[i].y == 0) { o[i].priority = 1; break; }
    return k;
}

/* xrandr --query: "DP-2 connected primary 1920x1080+1920+0 (…)". */
int plh_parse_xrandr(const char *txt, struct plh_output *o, int max)
{
    int n = 0;
    char *copy = strdup(txt), *save = NULL;
    if (!copy) return -1;
    for (char *l = strtok_r(copy, "\n", &save); l && n < max; l = strtok_r(NULL, "\n", &save)) {
        if (*l == ' ' || !strstr(l, " connected")) continue;
        char name[32]; int w, h, x, y;
        char *g = strstr(l, " connected");
        g += 10;
        int prim = 0;
        if (!strncmp(g, " primary", 8)) { prim = 1; g += 8; }
        if (sscanf(l, "%31s", name) != 1) continue;
        if (sscanf(g, " %dx%d+%d+%d", &w, &h, &x, &y) != 4) continue;
        memset(&o[n], 0, sizeof o[n]);
        snprintf(o[n].name, sizeof o[n].name, "%s", name);
        o[n].x = x; o[n].y = y; o[n].w = w; o[n].h = h; o[n].priority = prim;
        n++;
    }
    free(copy);
    return n;
}

/* swaymsg -t get_outputs -r : crude extraction of "name" then "rect":{"x","y","width","height"}. */
int plh_parse_sway_json(const char *txt, struct plh_output *o, int max)
{
    int n = 0;
    const char *p = txt;
    while (n < max && (p = strstr(p, "\"name\""))) {
        char name[32] = "";
        const char *q = strchr(p + 6, '"');
        if (!q) break;
        sscanf(q + 1, "%31[^\"]", name);
        const char *r = strstr(q, "\"rect\"");
        const char *nx = strstr(q, "\"name\"");
        if (!r || (nx && nx < r)) { p = q; continue; }
        int x, y, w, h;
        const char *a;
        if (!(a = strstr(r, "\"x\"")) || sscanf(a, "\"x\": %d", &x) != 1) break;
        if (!(a = strstr(r, "\"y\"")) || sscanf(a, "\"y\": %d", &y) != 1) break;
        if (!(a = strstr(r, "\"width\"")) || sscanf(a, "\"width\": %d", &w) != 1) break;
        if (!(a = strstr(r, "\"height\"")) || sscanf(a, "\"height\": %d", &h) != 1) break;
        memset(&o[n], 0, sizeof o[n]);
        snprintf(o[n].name, sizeof o[n].name, "%s", name);
        o[n].x = x; o[n].y = y; o[n].w = w; o[n].h = h;
        if (!x && !y) o[n].priority = 1;
        n++;
        p = r + 6;
    }
    return n;
}

static char *run_cmd(const char *cmd)
{
    FILE *f = popen(cmd, "r");
    if (!f) return NULL;
    size_t cap = 16384, len = 0;
    char *b = malloc(cap);
    if (!b) { pclose(f); return NULL; }
    size_t r;
    while (len + 1 < cap && (r = fread(b + len, 1, cap - len - 1, f)) > 0) len += r;
    b[len] = 0;
    return pclose(f) == 0 ? b : (free(b), NULL);
}

/* Automatic selection: KWin (kscreen-doctor) if KDE; else wlroots (wlr-randr, swaymsg);
 * else X11 (xrandr). Returns the number of outputs (0 = none), `src` = method used. */
int plh_detect_outputs(struct plh_output *o, int max, const char **src)
{
    const char *de = getenv("XDG_CURRENT_DESKTOP");
    int wl = getenv("WAYLAND_DISPLAY") != NULL;
    int n; char *t;
    *src = "none";
    if ((!de || strstr(de, "KDE")) && (n = plh_kwin_outputs(o, max)) > 0) { *src = "kwin"; return n; }
    if (wl) {
        if ((t = run_cmd("wlr-randr 2>/dev/null"))) { n = plh_parse_wlr_randr(t, o, max); free(t); if (n > 0) { *src = "wlr-randr"; return n; } }
        if ((t = run_cmd("swaymsg -t get_outputs -r 2>/dev/null"))) { n = plh_parse_sway_json(t, o, max); free(t); if (n > 0) { *src = "swaymsg"; return n; } }
    }
    if (getenv("DISPLAY") && (t = run_cmd("xrandr --query 2>/dev/null"))) {
        n = plh_parse_xrandr(t, o, max); free(t);
        if (n > 0) { *src = "xrandr"; return n; }
    }
    return 0;
}
