/* test-uinput-seq — regression: EXACT evdev event sequence emitted by the uinput backend,
 * on pipes (no device, no injection). TAP. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "plh_input.h"

static int n, fails;
#define CHECK(expr, desc) do { n++; if (expr) printf("ok %d - %s\n", n, desc); \
    else { fails++; printf("not ok %d - %s\n", n, desc); } } while (0)

static int p[3][2];
static char got[3][2048];

static void drain(void)
{
    for (int d = 0; d < 3; d++) {
        got[d][0] = 0;
        struct input_event ev;
        while (read(p[d][0], &ev, sizeof ev) == (ssize_t)sizeof ev) {
            char b[64];
            snprintf(b, sizeof b, "%u:%u:%d ", ev.type, ev.code, ev.value);
            strncat(got[d], b, sizeof got[d] - strlen(got[d]) - 1);
        }
    }
}

int main(void)
{
    for (int d = 0; d < 3; d++) { if (pipe2(p[d], O_NONBLOCK)) return 1; }
    plh_rect desk = { 0, 0, 5760, 1080 };
    plh_input *in = plh_uinput_wrap_fds(&desk, p[0][1], p[1][1], p[2][1]);
    char exp[256];

    /* scenario from critique: abs 2880 540 then left click */
    plh_input_move_abs(in, 2880, 540);
    plh_input_button(in, PLH_BTN_LEFT, 1);
    plh_input_button(in, PLH_BTN_LEFT, 0);
    drain();
    snprintf(exp, sizeof exp, "%d:%d:2880 %d:%d:540 0:0:0 1:%d:1 0:0:0 1:%d:0 0:0:0 ",
             EV_ABS, ABS_X, EV_ABS, ABS_Y, BTN_LEFT, BTN_LEFT);
    printf("# abs : %s\n", got[2]);
    CHECK(!strcmp(got[2], exp), "abs 2880,540 + clic : ABS_X 2880, ABS_Y 540, SYN, BTN_LEFT 1, SYN, BTN_LEFT 0, SYN (rien d'autre)");
    CHECK(!got[0][0] && !got[1][0], "no event on the keyboard or the relative mouse");
    CHECK(!strstr(got[2], "3:0:0 ") && !strstr(got[2], "3:1:0 "), "jamais ABS_X=0 ni ABS_Y=0");

    /* same point repeated after a relative move: neighbour then target, never 0 */
    plh_input_move_rel(in, 5, 0);
    plh_input_move_abs(in, 2880, 540);
    drain();
    printf("# rel : %s | abs : %s\n", got[1], got[2]);
    snprintf(exp, sizeof exp, "%d:%d:2879 0:0:0 %d:%d:2880 %d:%d:540 0:0:0 ", EV_ABS, ABS_X, EV_ABS, ABS_X, EV_ABS, ABS_Y);
    CHECK(!strcmp(got[2], exp), "return to the same point : ABS_X 2879, SYN, ABS_X 2880, ABS_Y 540, SYN");
    snprintf(exp, sizeof exp, "%d:%d:5 0:0:0 ", EV_REL, REL_X);
    CHECK(!strcmp(got[1], exp), "rel +5,0 : REL_X 5, SYN (no REL_Y)");

    /* the button follows the last pointer moved (relative here) */
    plh_input_move_rel(in, 1, 1);
    plh_input_button(in, PLH_BTN_RIGHT, 1);
    plh_input_button(in, PLH_BTN_RIGHT, 0);
    drain();
    CHECK(!got[2][0], "after a relative move, the click does not touch the absolute pointer");
    CHECK(strstr(got[1], "1:273:1 ") != NULL, "right click emitted on the relative mouse");

    /* clamping: never outside the desktop */
    plh_input_move_abs(in, -50, 99999);
    drain();
    snprintf(exp, sizeof exp, "%d:%d:0 %d:%d:1079 0:0:0 ", EV_ABS, ABS_X, EV_ABS, ABS_Y);
    CHECK(!strcmp(got[2], exp), "coordinates clamped to the desktop (0,1079)");

    /* desktop offset (--desktop X,Y) */
    plh_rect d2 = { 100, 50, 1920, 1080 };
    plh_input *in2 = plh_uinput_wrap_fds(&d2, p[0][1], p[1][1], p[2][1]);
    plh_input_move_abs(in2, 1060, 590);
    drain();
    snprintf(exp, sizeof exp, "%d:%d:960 %d:%d:540 0:0:0 ", EV_ABS, ABS_X, EV_ABS, ABS_Y);
    CHECK(!strcmp(got[2], exp), "offset desktop 100,50: 1060,590 -> ABS 960,540");
    printf("1..%d\n", n);
    return fails != 0;
}
