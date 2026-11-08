/* vout.c — virtual display: lifecycle of the KWin virtual output and DISPLAY_STATE. Owner: capture.
 * At most one virtual output. Owners (client connections) are counted; the output goes away when the last owner
 * destroys it or disconnects, or when the bridge exits (KWin drops it if the bridge dies: the output belongs to
 * the stream). Settings come from the environment of the bridge and are the single source of truth. */
#define _GNU_SOURCE
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "cap_internal.h"

#define MOD "vout"
#define MAX_OWNERS 16

static struct plh_client *owners[MAX_OWNERS];
static int nowners;
static struct plh_vout_create_reply cur; /* valid while cap_kwin_vout_alive() */

static int env_flag(const char *n)
{
    const char *v = getenv(n);
    return v && v[0] && strcmp(v, "0") && strcmp(v, "false") && strcmp(v, "no");
}

static void parse_mode(uint32_t *w, uint32_t *h, uint32_t *r)
{
    unsigned ww = 1920, hh = 1080, rr = 60;
    const char *m = getenv("PLH_VIRTUAL_DISPLAY_MODE");
    if (m && m[0]) {
        unsigned a = 0, b = 0, c = 0;
        int n = sscanf(m, "%ux%u@%u", &a, &b, &c);
        if (n >= 2 && a >= 320 && b >= 200 && a <= 8192 && b <= 8192) { ww = a; hh = b; if (n == 3 && c >= 24 && c <= 240) rr = c; }
        else PLH_WARN(MOD, "bad PLH_VIRTUAL_DISPLAY_MODE '%s', using 1920x1080", m);
    }
    if (ww > PLH_VOUT_MAX_WIDTH) {   /* H.264 limit of the encoder */
        hh = (unsigned)((unsigned long long)hh * PLH_VOUT_MAX_WIDTH / ww) & ~1u;
        ww = PLH_VOUT_MAX_WIDTH;
    }
    *w = ww; *h = hh; *r = rr * 1000;
}

static int find_virtual(struct plh_vout_create_reply *r)
{
    struct plh_cap_output outs[CAP_MAX_OUTPUTS];
    int n = cap_outputs_query(outs, CAP_MAX_OUTPUTS);
    for (int i = 0; i < n; i++)
        if (outs[i].flags & PLH_CAP_OUTPUT_VIRTUAL) {
            r->output_id = outs[i].output_id; r->x = outs[i].x; r->y = outs[i].y;
            r->width = outs[i].width; r->height = outs[i].height; r->refresh_mhz = outs[i].refresh_mhz;
            snprintf(r->name, sizeof r->name, "%s", outs[i].name);
            return 1;
        }
    return 0;
}

static void lost(void)
{
    PLH_WARN(MOD, "virtual output lost (KWin closed it)");
    memset(&cur, 0, sizeof cur);
}

static int owner_idx(struct plh_client *c)
{
    for (int i = 0; i < nowners; i++) if (owners[i] == c) return i;
    return -1;
}

static void drop_if_unowned(void)
{
    if (nowners == 0 && cap_kwin_vout_alive()) {
        cap_kwin_vout_stop();
        memset(&cur, 0, sizeof cur);
        PLH_INFO(MOD, "virtual output removed");
    }
}

static int create(struct plh_client *c, const struct plh_vout_create *rq, struct plh_vout_create_reply *rep)
{
    if (!cap_kwin_vout_alive()) {
        uint32_t w, h, r;
        parse_mode(&w, &h, &r);
        if (rq->width && rq->height) {
            w = rq->width > PLH_VOUT_MAX_WIDTH ? PLH_VOUT_MAX_WIDTH : rq->width; h = rq->height;
            if (w < 320 || h < 200 || h > 8192) return PLH_E_INVAL;
        }
        memset(&cur, 0, sizeof cur);
        int rc = cap_kwin_vout_start(PLH_VOUT_NAME, PLH_VOUT_DESCRIPTION, w, h, lost);
        if (rc) return rc;
        /* the output shows up asynchronously: poll the compositor (<= 3 s) */
        /* KWin first shows the output at 0,0 and then moves it right of the others: wait until the rect is
         * stable for 3 consecutive polls (or give up waiting after 3 s and report the last rect) */
        int ok = 0, stable = 0;
        struct plh_vout_create_reply prev = { 0 };
        for (int i = 0; i < 60 && stable < 3 && cap_kwin_vout_alive(); i++) {
            struct plh_vout_create_reply now = { 0 };
            if (find_virtual(&now)) {
                ok = 1;
                cur = now;
                stable = (now.x == prev.x && now.y == prev.y && now.width == prev.width && now.height == prev.height) ? stable + 1 : 0;
                prev = now;
            }
            if (stable < 3) { struct timespec ts = { 0, 100 * 1000000L }; nanosleep(&ts, NULL); }
        }
        if (!ok) { cap_kwin_vout_stop(); memset(&cur, 0, sizeof cur); return PLH_E_IO; }
        PLH_INFO(MOD, "virtual output created: %s %d,%d %ux%u", cur.name, cur.x, cur.y, cur.width, cur.height);
    } else {
        find_virtual(&cur); /* refresh the rect */
    }
    if (owner_idx(c) < 0) {
        if (nowners >= MAX_OWNERS) return PLH_E_BUSY;
        owners[nowners++] = c;
    }
    *rep = cur;
    rep->status = PLH_OK;
    return PLH_OK;
}

/* real outputs: DPMS from the kernel (readable without privileges), connector name = KWin output name */
static int connector_dpms_on(const char *name)
{
    DIR *d = opendir("/sys/class/drm");
    int res = -1;
    if (!d) return -1;
    struct dirent *e;
    size_t nl = strlen(name);
    while ((e = readdir(d))) {
        const char *dash = strchr(e->d_name, '-');
        if (strncmp(e->d_name, "card", 4) || !dash || strcmp(dash + 1, name) || strlen(dash + 1) != nl) continue;
        char p[300], buf[32] = "";
        snprintf(p, sizeof p, "/sys/class/drm/%s/dpms", e->d_name);
        FILE *f = fopen(p, "r");
        if (!f) continue;
        if (fgets(buf, sizeof buf, f)) res = !strncmp(buf, "On", 2);
        fclose(f);
        break;
    }
    closedir(d);
    return res;
}

static void fill_state(struct plh_display_state *s)
{
    memset(s, 0, sizeof *s);
    s->flags = (env_flag("PLH_VIRTUAL_DISPLAY") ? PLH_DS_OFFER : 0) | (env_flag("PLH_VIRTUAL_DISPLAY_AUTO") ? PLH_DS_AUTO : 0);
    int fake = env_flag("PLH_FAKE_SCREENS_OFF");
    if (fake) s->flags |= PLH_DS_FAKE_OFF;
    parse_mode(&s->mode_width, &s->mode_height, &s->mode_refresh_mhz);
    struct plh_cap_output outs[CAP_MAX_OUTPUTS];
    int n = cap_outputs_query(outs, CAP_MAX_OUTPUTS);
    int right_set = 0;
    for (int i = 0; i < n; i++) {
        if (outs[i].flags & PLH_CAP_OUTPUT_VIRTUAL) {
            s->virtual_active = 1; s->virtual_output_id = outs[i].output_id;
            s->vx = outs[i].x; s->vy = outs[i].y; s->vwidth = outs[i].width; s->vheight = outs[i].height;
            continue;
        }
        s->real_total++;
        if (fake || connector_dpms_on(outs[i].name) == 0) s->real_off++;
        else s->real_on++;
        int32_t r = outs[i].x + (int32_t)outs[i].width;
        if (!right_set || r > s->real_right) { s->real_right = r; right_set = 1; }
    }
}

void cap_vout_handle(struct plh_client *c, const struct plh_msg_hdr *h, const void *p)
{
    switch (h->type) {
    case PLH_MSG_VOUT_CREATE: {
        struct plh_vout_create rq = { 0 };
        memcpy(&rq, p, h->size < sizeof rq ? h->size : sizeof rq);
        struct plh_vout_create_reply rep = { 0 };
        if (!env_flag("PLH_VIRTUAL_DISPLAY") && !env_flag("PLH_VIRTUAL_DISPLAY_AUTO")) {
            PLH_WARN(MOD, "VOUT_CREATE refused: PLH_VIRTUAL_DISPLAY and PLH_VIRTUAL_DISPLAY_AUTO are both off");
            plh_reply_error(c, h, PLH_E_PERM, "virtual display disabled (PLH_VIRTUAL_DISPLAY=0)");
            return;
        }
        int rc = create(c, &rq, &rep);
        if (rc) { plh_reply_error(c, h, rc, "cannot create the virtual output (%d)", rc); return; }
        plh_reply(c, h, &rep, sizeof rep);
        break;
    }
    case PLH_MSG_VOUT_DESTROY: {
        struct plh_vout_ref ref = { 0 };
        memcpy(&ref, p, h->size < sizeof ref ? h->size : sizeof ref);
        int i = owner_idx(c);
        if (i < 0) { plh_reply_error(c, h, PLH_E_NOENT, "not an owner of a virtual output"); return; }
        owners[i] = owners[--nowners];
        drop_if_unowned();
        if (h->seq) plh_reply(c, h, NULL, 0);
        break;
    }
    case PLH_MSG_DISPLAY_STATE: {
        struct plh_display_state s;
        fill_state(&s);
        plh_reply(c, h, &s, sizeof s);
        break;
    }
    }
}

void cap_vout_client_gone(struct plh_client *c)
{
    int i = owner_idx(c);
    if (i < 0) return;
    owners[i] = owners[--nowners];
    PLH_INFO(MOD, "owner client %u gone (%d owners left)", plh_client_id(c), nowners);
    drop_if_unowned();
}

void cap_vout_fini(void)
{
    nowners = 0;
    cap_kwin_vout_stop();
}
