/* Layout parsers on sample outputs (no injection). */
#include <stdio.h>
#include <string.h>
#include "plh_input.h"
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
int main(void)
{
    struct plh_output o[8];
    int n = plh_parse_wlr_randr("DP-1 \"Dell\"\n  Enabled: yes\n  Modes:\n    1280x720 px, 60 Hz\n    2560x1440 px, 60 Hz (current)\n  Position: 0,0\n"
                                "HDMI-A-1 \"x\"\n  Enabled: yes\n  Modes:\n    1920x1080 px, 60 Hz (current)\n  Position: 2560,0\n"
                                "eDP-1 \"off\"\n  Enabled: no\n  Modes:\n    1920x1080 px, 60 Hz (current)\n  Position: 0,0\n", o, 8);
    CHECK(n == 2); CHECK(o[0].w == 2560 && o[0].h == 1440 && o[0].priority == 1); CHECK(o[1].x == 2560 && o[1].w == 1920);
    plh_rect b = plh_outputs_bounds(o, n); CHECK(b.w == 4480 && b.h == 1440);
    n = plh_parse_xrandr("Screen 0: minimum 8 x 8, current 5760 x 1080, maximum 32767 x 32767\nHDMI-1 connected 1920x1080+0+0 (normal) 509mm x 286mm\n"
                         "   1920x1080     60.00*+\nDP-2 connected primary 1920x1080+1920+0 (normal left) 509mm\nDP-3 disconnected (normal)\n", o, 8);
    CHECK(n == 2); CHECK(o[1].x == 1920 && o[1].priority == 1 && !strcmp(o[1].name, "DP-2"));
    n = plh_parse_sway_json("[{\"name\": \"DP-1\", \"rect\": {\"x\": 0, \"y\": 0, \"width\": 1920, \"height\": 1080}},"
                            "{\"name\": \"DP-2\", \"rect\": {\"x\": -1080, \"y\": -200, \"width\": 1080, \"height\": 1920}}]", o, 8);
    CHECK(n == 2); CHECK(o[1].x == -1080 && o[1].h == 1920 && o[0].priority == 1);
    b = plh_outputs_bounds(o, n); CHECK(b.x == -1080 && b.y == -200 && b.w == 3000 && b.h == 1920);
    puts(fails ? "layout: FAIL" : "layout: ok");
    return fails != 0;
}
