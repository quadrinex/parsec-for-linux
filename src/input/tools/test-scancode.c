/* test-scancode — checks the conversion tables (TAP output, no injection). */
#include <linux/input-event-codes.h>
#include <stdio.h>
#include "plh_input.h"

static int n, fails;
#define CHECK(expr, desc) do { n++; if (expr) printf("ok %d - %s\n", n, desc); \
    else { fails++; printf("not ok %d - %s\n", n, desc); } } while (0)

int main(void)
{
    /* scancodes jeu 1 */
    CHECK(plh_scancode_to_evdev(0x1e, 0) == KEY_A, "sc 1E -> KEY_A");
    CHECK(plh_scancode_to_evdev(0x1c, 0) == KEY_ENTER, "sc 1C -> ENTER");
    CHECK(plh_scancode_to_evdev(0x1c, 1) == KEY_KPENTER, "sc E0 1C -> KPENTER");
    CHECK(plh_scancode_to_evdev(0xe01d, 0) == KEY_RIGHTCTRL, "sc 0xE01D -> RIGHTCTRL");
    CHECK(plh_scancode_to_evdev(0x38, 1) == KEY_RIGHTALT, "sc E0 38 -> RIGHTALT (AltGr)");
    CHECK(plh_scancode_to_evdev(0x48, 1) == KEY_UP && plh_scancode_to_evdev(0x48, 0) == KEY_KP8, "up arrow vs keypad 8");
    CHECK(plh_scancode_to_evdev(0x5b, 1) == KEY_LEFTMETA, "sc E0 5B -> LEFTMETA");
    CHECK(plh_scancode_to_evdev(0x56, 0) == KEY_102ND, "sc 56 -> 102ND (<> key on AZERTY)");
    CHECK(plh_scancode_to_evdev(0x57, 0) == KEY_F11 && plh_scancode_to_evdev(0x58, 0) == KEY_F12, "F11/F12");
    CHECK(plh_scancode_to_evdev(0x76, 0) == KEY_F24, "sc 76 -> F24");
    CHECK(plh_scancode_to_evdev(0xe11d, 0) == KEY_PAUSE, "sc E1 1D -> PAUSE");
    CHECK(plh_scancode_to_evdev(0x37, 1) == KEY_SYSRQ, "sc E0 37 -> SYSRQ");
    CHECK(plh_scancode_to_evdev(0x200, 0) == 0, "scancode hors table -> 0");
    /* all non-extended scancodes 01..53 = numeric evdev identity */
    int id = 1;
    for (unsigned s = 1; s <= 0x53; s++) if (plh_scancode_to_evdev(s, 0) != (int)s) id = 0;
    CHECK(id, "sc 01..53 identical to evdev codes");
    /* VK */
    CHECK(plh_vk_to_evdev(0x41, 0) == KEY_A, "VK_A");
    CHECK(plh_vk_to_evdev(0x0d, 1) == KEY_KPENTER, "extended VK_RETURN -> KPENTER");
    CHECK(plh_vk_to_evdev(0x11, 1) == KEY_RIGHTCTRL, "extended VK_CONTROL -> RIGHTCTRL");
    CHECK(plh_vk_to_evdev(0x87, 0) == KEY_F24, "VK_F24");
    CHECK(plh_vk_to_evdev(0xa1, 0) == KEY_RIGHTSHIFT, "VK_RSHIFT");
    CHECK(plh_vk_to_evdev(0xe2, 0) == KEY_102ND, "VK_OEM_102");
    CHECK(plh_evdev_from_name("F24") == KEY_F24 && plh_evdev_from_name("KEY_LEFTSHIFT") == KEY_LEFTSHIFT
          && plh_evdev_from_name("F5") == KEY_F5 && plh_evdev_from_name("a") == KEY_A, "noms de touches");
    /* normalized absolute coordinates, wineserver arithmetic */
    plh_rect vs = { 0, 0, 5760, 1080 };
    int x, y;
    plh_norm65535_to_px(&vs, 0, 0, &x, &y);        CHECK(x == 0 && y == 0, "norm 0,0 -> 0,0");
    plh_norm65535_to_px(&vs, 65535, 65535, &x, &y); CHECK(x == 5759 && y == 1079, "norm max -> 5759,1079");
    plh_norm65535_to_px(&vs, 32768, 32768, &x, &y); CHECK(x == 2880 && y == 540, "norm milieu -> 2880,540");
    /* Windows -> KWin translation: sample layout (KWin primary at 1920,0) */
    struct plh_output k[3] = { {"HDMI-A-1", 0, 0, 1920, 1080, 3}, {"HDMI-A-2", 3840, 0, 1920, 1080, 2},
                               {"DP-2", 1920, 0, 1920, 1080, 1} };
    plh_rect w[3] = { {0, 0, 1920, 1080}, {1920, 0, 1920, 1080}, {-1920, 0, 1920, 1080} };
    int tx = 0, ty = 0;
    CHECK(plh_layout_translation(w, 3, 0, k, 3, &tx, &ty) == 0 && tx == 1920 && ty == 0, "translation Windows->KWin = +1920,0");
    plh_rect wbad[1] = { {0, 0, 2560, 1440} };
    CHECK(plh_layout_translation(wbad, 1, 0, k, 3, &tx, &ty) == -1, "incompatible layout detected");
    plh_rect w1[1] = { {0, 0, 1920, 1080} };
    CHECK(plh_layout_translation(w1, 1, 0, k, 3, &tx, &ty) == 0 && tx == 1920, "ambiguity: prefers the KWin primary output");
    plh_rect b = plh_outputs_bounds(k, 3);
    CHECK(b.x == 0 && b.y == 0 && b.w == 5760 && b.h == 1080, "rectangle englobant 5760x1080");
    struct plh_output live[8];
    int nl = plh_kwin_outputs(live, 8);
    printf("# kscreen-doctor : %d sorties\n", nl);
    for (int i = 0; i < nl; i++) printf("#   %s %d,%d %dx%d prio %d\n", live[i].name, live[i].x, live[i].y, live[i].w, live[i].h, live[i].priority);
    printf("1..%d\n", n);
    return fails != 0;
}
