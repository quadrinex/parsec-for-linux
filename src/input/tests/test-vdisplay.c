/* Unit tests, no injection: virtual-output handling in layout.c, redirect mapping, uinput resize
 * logic (on pipes, no device created). */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "plh_input.h"
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static struct plh_output mk(const char *n, int x, int y, int w, int h, int prio)
{ struct plh_output o; memset(&o, 0, sizeof o); snprintf(o.name, sizeof o.name, "%s", n); o.x = x; o.y = y; o.w = w; o.h = h; o.priority = prio; return o; }

int main(void)
{
    struct plh_output k[4] = { mk("HDMI-A-1", 0, 0, 1920, 1080, 2), mk("DP-2", 1920, 0, 1920, 1080, 1), mk("HDMI-A-2", 3840, 0, 1920, 1080, 3), mk("Virtual-plh-virtual", 5760, 0, 1920, 1080, 4) };
    CHECK(plh_output_is_virtual("Virtual-plh-virtual") && plh_output_is_virtual("plh-virtual") && !plh_output_is_virtual("DP-2"));

    /* desk refresh: 3 outputs -> 4 (virtual added) -> 3 (removed) */
    plh_rect d; int tx, ty;
    CHECK(plh_layout_snapshot(k, 3, &d, &tx, &ty) == 0 && d.w == 5760 && tx == 1920 && ty == 0);
    CHECK(plh_layout_snapshot(k, 4, &d, &tx, &ty) == 0 && d.x == 0 && d.w == 7680 && d.h == 1080 && tx == 1920);
    /* virtual output claiming priority 1 / being first must not become the primary */
    struct plh_output k2[4] = { k[3], k[0], k[1], k[2] }; k2[0].priority = 1; k2[2].priority = 2;
    CHECK(plh_layout_snapshot(k2, 4, &d, &tx, &ty) == 0 && tx == 0 && ty == 0 && d.w == 7680);
    CHECK(plh_kwin_primary_translation(k2, 4, &tx, &ty) != 0 || (tx == 0));

    /* translation: Wine primary 1920x1080 at 0,0, others at -1920 and +1920 (virtual not listed by Wine) */
    plh_rect w3[3] = { {0,0,1920,1080}, {1920,0,1920,1080}, {-1920,0,1920,1080} };
    CHECK(plh_layout_translation(w3, 3, 0, k, 4, &tx, &ty) == 0 && tx == 1920 && ty == 0);
    /* Wine lists the virtual output as well (Wine follows XWayland live) */
    plh_rect w4[4] = { {0,0,1920,1080}, {1920,0,1920,1080}, {-1920,0,1920,1080}, {3840,0,1920,1080} };
    CHECK(plh_layout_translation(w4, 4, 0, k, 4, &tx, &ty) == 0 && tx == 1920);
    /* Windows primary of the virtual size must still anchor on the real primary, never on the virtual output */
    struct plh_output k5[2] = { mk("DP-2", 0, 0, 1920, 1080, 1), mk("Virtual-plh-virtual", 1920, 0, 1920, 1080, 0) };
    plh_rect w5[1] = { {0,0,1920,1080} };
    CHECK(plh_layout_translation(w5, 1, 0, k5, 2, &tx, &ty) == 0 && tx == 0);

    /* KWin moves the primary (e.g. a screen is added on the left): client translation follows */
    { int ctx = 1920, cty = 0;
      plh_translation_shift(&ctx, &cty, 1920, 0, 3840, 0); CHECK(ctx == 3840 && cty == 0);
      plh_translation_shift(&ctx, &cty, 3840, 0, 1920, 0); CHECK(ctx == 1920);
      plh_translation_shift(&ctx, &cty, 1920, 0, 1920, 0); CHECK(ctx == 1920); }

    /* redirect mapping */
    struct plh_redir r[2] = { { {0,0,1920,1080}, {5760,0,1920,1080} }, { {100,100,100,100}, {0,0,200,200} } };
    int kx, ky;
    CHECK(plh_redirect_map(r, 2, 10, 20, &kx, &ky) == 1 && kx == 5770 && ky == 20);       /* first entry wins */
    CHECK(plh_redirect_map(r, 2, 1919, 1079, &kx, &ky) == 1 && kx == 7679 && ky == 1079);
    CHECK(plh_redirect_map(r, 2, 1920, 0, &kx, &ky) == 0);                                  /* right edge exclusive */
    CHECK(plh_redirect_map(r + 1, 1, 150, 150, &kx, &ky) == 1 && kx == 100 && ky == 100);   /* scaled */
    CHECK(plh_redirect_map(r, 0, 10, 20, &kx, &ky) == 0);
    struct plh_redir bad = { {0,0,0,0}, {0,0,10,10} };
    CHECK(plh_redirect_map(&bad, 1, 0, 0, &kx, &ky) == 0);

    /* uinput resize: unchanged / applied / deferred while a button is down / applied on release */
    int p[3][2];
    for (int i = 0; i < 3; i++) if (pipe(p[i])) return 2;
    plh_rect desk = { 0, 0, 5760, 1080 }, bigger = { 0, 0, 7680, 1080 };
    plh_input *in = plh_uinput_wrap_fds(&desk, p[0][1], p[1][1], p[2][1]);
    CHECK(plh_uinput_resize(in, &desk) == 0);
    CHECK(plh_input_button(in, PLH_BTN_LEFT, 1) == 0);
    CHECK(plh_uinput_resize(in, &bigger) == 1 && in->desktop.w == 5760);
    CHECK(plh_input_button(in, PLH_BTN_LEFT, 0) == 0 && in->desktop.w == 7680);
    CHECK(plh_uinput_resize(in, &desk) == 0 && in->desktop.w == 5760);
    plh_rect bad2 = { 0, 0, 0, 10 };
    CHECK(plh_uinput_resize(in, &bad2) < 0);
    /* a deferred resize cancelled by a return to the current size */
    CHECK(plh_input_button(in, PLH_BTN_RIGHT, 1) == 0 && plh_uinput_resize(in, &bigger) == 1 && plh_uinput_resize(in, &desk) == 0);
    CHECK(plh_input_button(in, PLH_BTN_RIGHT, 0) == 0 && in->desktop.w == 5760);
    plh_input_close(in);
    puts(fails ? "vdisplay: FAIL" : "vdisplay: ok");
    return fails != 0;
}
