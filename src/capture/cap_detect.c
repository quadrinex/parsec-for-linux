/* cap_detect.c — backend detection from the session, and output enumeration.
 * Owner: capture. Nothing hard-coded: everything comes from the environment and the display server. */
#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "cap_internal.h"

#define MOD "detect"

static int has_word(const char *list, const char *w)
{
    if (!list) return 0;
    size_t n = strlen(w);
    for (const char *p = list; (p = strcasestr(p, w)); p += n)
        if ((p == list || p[-1] == ':') && (p[n] == 0 || p[n] == ':')) return 1;
    return 0;
}

static const char *session_kind(void)
{
    const char *t = getenv("XDG_SESSION_TYPE");
    if (t && !strcasecmp(t, "wayland")) return "wayland";
    if (t && !strcasecmp(t, "x11")) return "x11";
    if (getenv("WAYLAND_DISPLAY")) return "wayland";
    if (getenv("DISPLAY")) return "x11";
    return "";
}

int cap_detect_order(const struct cap_backend_ops **order, int max)
{
    int n = 0;
    const char *kind = session_kind();
    const char *de = getenv("XDG_CURRENT_DESKTOP");
    if (max < 4) return 0;
    if (!strcmp(kind, "wayland")) {
        /* KDE: zkde_screencast with no dialog (presence checked at bind, falls back to the portal);
         * GNOME, wlroots (xdg-desktop-portal-wlr), others: ScreenCast portal */
        if (has_word(de, "KDE")) order[n++] = &cap_backend_kwin;
        order[n++] = &cap_backend_portal;
    } else if (!strcmp(kind, "x11")) {
        order[n++] = &cap_backend_x11;
    } else {
        order[n++] = &cap_backend_kwin;
        order[n++] = &cap_backend_portal;
        order[n++] = &cap_backend_x11;
    }
    order[n] = NULL;
    PLH_INFO(MOD, "session=%s desktop=%s first backend=%s", kind[0] ? kind : "?", de ? de : "?", order[0]->name);
    return n;
}

int cap_outputs_query(struct plh_cap_output *out, int max)
{
    int n;
    if (!strcmp(session_kind(), "x11")) n = cap_outputs_query_x11(out, max);
    else {
        n = cap_outputs_query_wl(out, max);
        if (n < 0 && getenv("DISPLAY")) n = cap_outputs_query_x11(out, max);
    }
    for (int i = 0; i < n; i++)
        if (!strcmp(out[i].name, CAP_VOUT_CONNECTOR)) out[i].flags |= PLH_CAP_OUTPUT_VIRTUAL;
    return n;
}
