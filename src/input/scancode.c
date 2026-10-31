/* scancode.c — Windows scancodes (set 1, + E0 prefix) and VK_* -> evdev KEY_* codes.
 * Public sources: "Keyboard Scan Code Specification" (Microsoft), winuser.h,
 * linux/input-event-codes.h. */
#include <linux/input-event-codes.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdlib.h>
#include "plh_input.h"

/* Non-extended scancodes 0x00..0x7F. For 0x01..0x58, the evdev code is numerically
 * identical to the set 1 scancode (kernel AT heritage), except for the exceptions below. */
static const unsigned short sc_plain[0x80] = {
    [0x01] = KEY_ESC, [0x02] = KEY_1, [0x03] = KEY_2, [0x04] = KEY_3, [0x05] = KEY_4,
    [0x06] = KEY_5, [0x07] = KEY_6, [0x08] = KEY_7, [0x09] = KEY_8, [0x0a] = KEY_9,
    [0x0b] = KEY_0, [0x0c] = KEY_MINUS, [0x0d] = KEY_EQUAL, [0x0e] = KEY_BACKSPACE,
    [0x0f] = KEY_TAB, [0x10] = KEY_Q, [0x11] = KEY_W, [0x12] = KEY_E, [0x13] = KEY_R,
    [0x14] = KEY_T, [0x15] = KEY_Y, [0x16] = KEY_U, [0x17] = KEY_I, [0x18] = KEY_O,
    [0x19] = KEY_P, [0x1a] = KEY_LEFTBRACE, [0x1b] = KEY_RIGHTBRACE, [0x1c] = KEY_ENTER,
    [0x1d] = KEY_LEFTCTRL, [0x1e] = KEY_A, [0x1f] = KEY_S, [0x20] = KEY_D, [0x21] = KEY_F,
    [0x22] = KEY_G, [0x23] = KEY_H, [0x24] = KEY_J, [0x25] = KEY_K, [0x26] = KEY_L,
    [0x27] = KEY_SEMICOLON, [0x28] = KEY_APOSTROPHE, [0x29] = KEY_GRAVE,
    [0x2a] = KEY_LEFTSHIFT, [0x2b] = KEY_BACKSLASH, [0x2c] = KEY_Z, [0x2d] = KEY_X,
    [0x2e] = KEY_C, [0x2f] = KEY_V, [0x30] = KEY_B, [0x31] = KEY_N, [0x32] = KEY_M,
    [0x33] = KEY_COMMA, [0x34] = KEY_DOT, [0x35] = KEY_SLASH, [0x36] = KEY_RIGHTSHIFT,
    [0x37] = KEY_KPASTERISK, [0x38] = KEY_LEFTALT, [0x39] = KEY_SPACE,
    [0x3a] = KEY_CAPSLOCK, [0x3b] = KEY_F1, [0x3c] = KEY_F2, [0x3d] = KEY_F3,
    [0x3e] = KEY_F4, [0x3f] = KEY_F5, [0x40] = KEY_F6, [0x41] = KEY_F7, [0x42] = KEY_F8,
    [0x43] = KEY_F9, [0x44] = KEY_F10, [0x45] = KEY_NUMLOCK, [0x46] = KEY_SCROLLLOCK,
    [0x47] = KEY_KP7, [0x48] = KEY_KP8, [0x49] = KEY_KP9, [0x4a] = KEY_KPMINUS,
    [0x4b] = KEY_KP4, [0x4c] = KEY_KP5, [0x4d] = KEY_KP6, [0x4e] = KEY_KPPLUS,
    [0x4f] = KEY_KP1, [0x50] = KEY_KP2, [0x51] = KEY_KP3, [0x52] = KEY_KP0,
    [0x53] = KEY_KPDOT, [0x54] = KEY_SYSRQ /* Alt+PrintScreen */, [0x56] = KEY_102ND,
    [0x57] = KEY_F11, [0x58] = KEY_F12, [0x59] = KEY_KPEQUAL,
    [0x64] = KEY_F13 /* spec MS jeu 1 */, [0x65] = KEY_F14,
    [0x66] = KEY_F15, [0x67] = KEY_F16, [0x68] = KEY_F17, [0x69] = KEY_F18,
    [0x6a] = KEY_F19, [0x6b] = KEY_F20, [0x6c] = KEY_F21, [0x6d] = KEY_F22,
    [0x6e] = KEY_F23,
    [0x70] = KEY_KATAKANAHIRAGANA, [0x73] = KEY_RO, [0x76] = KEY_F24,
    [0x79] = KEY_HENKAN,
    [0x7b] = KEY_MUHENKAN, [0x7d] = KEY_YEN, [0x7e] = KEY_KPCOMMA,
};

/* Extended scancodes (E0 prefix). */
static const unsigned short sc_e0[0x80] = {
    [0x10] = KEY_PREVIOUSSONG, [0x19] = KEY_NEXTSONG, [0x1c] = KEY_KPENTER,
    [0x1d] = KEY_RIGHTCTRL, [0x20] = KEY_MUTE, [0x21] = KEY_CALC, [0x22] = KEY_PLAYPAUSE,
    [0x24] = KEY_STOPCD, [0x2e] = KEY_VOLUMEDOWN, [0x30] = KEY_VOLUMEUP,
    [0x32] = KEY_HOMEPAGE, [0x35] = KEY_KPSLASH, [0x37] = KEY_SYSRQ /* PrintScreen */,
    [0x38] = KEY_RIGHTALT, [0x45] = KEY_NUMLOCK /* SendInput sometimes puts E0 on NumLock */,
    [0x46] = KEY_PAUSE /* Ctrl+Pause */, [0x47] = KEY_HOME, [0x48] = KEY_UP,
    [0x49] = KEY_PAGEUP, [0x4b] = KEY_LEFT, [0x4d] = KEY_RIGHT, [0x4f] = KEY_END,
    [0x50] = KEY_DOWN, [0x51] = KEY_PAGEDOWN, [0x52] = KEY_INSERT, [0x53] = KEY_DELETE,
    [0x5b] = KEY_LEFTMETA, [0x5c] = KEY_RIGHTMETA, [0x5d] = KEY_COMPOSE /* Menu */,
    [0x5e] = KEY_POWER, [0x5f] = KEY_SLEEP, [0x63] = KEY_WAKEUP, [0x65] = KEY_SEARCH,
    [0x66] = KEY_BOOKMARKS, [0x67] = KEY_REFRESH, [0x68] = KEY_STOP, [0x69] = KEY_FORWARD,
    [0x6a] = KEY_BACK, [0x6b] = KEY_COMPUTER, [0x6c] = KEY_MAIL, [0x6d] = KEY_MEDIA,
};

int plh_scancode_to_evdev(unsigned scan, int extended)
{
    /* Also accepts the form 0xE0xx / 0xE1xx. */
    if ((scan & 0xff00) == 0xe000) { extended = 1; scan &= 0xff; }
    else if ((scan & 0xff00) == 0xe100) { return (scan & 0xff) == 0x1d ? KEY_PAUSE : 0; }
    if (!extended && scan == 0xf1) return KEY_HANJA;
    if (!extended && scan == 0xf2) return KEY_HANGEUL;
    if (scan >= 0x80) return 0;
    return extended ? sc_e0[scan] : sc_plain[scan];
}

/* VK_* (winuser.h) -> evdev, physical US layout (like MapVirtualKey on a US keyboard). */
static const unsigned short vk_tab[256] = {
    [0x08] = KEY_BACKSPACE, [0x09] = KEY_TAB, [0x0c] = KEY_KP5 /* CLEAR */,
    [0x0d] = KEY_ENTER, [0x10] = KEY_LEFTSHIFT, [0x11] = KEY_LEFTCTRL, [0x12] = KEY_LEFTALT,
    [0x13] = KEY_PAUSE, [0x14] = KEY_CAPSLOCK, [0x15] = KEY_HANGEUL, [0x19] = KEY_HANJA,
    [0x1b] = KEY_ESC, [0x1c] = KEY_HENKAN, [0x1d] = KEY_MUHENKAN, [0x20] = KEY_SPACE,
    [0x21] = KEY_PAGEUP, [0x22] = KEY_PAGEDOWN, [0x23] = KEY_END, [0x24] = KEY_HOME,
    [0x25] = KEY_LEFT, [0x26] = KEY_UP, [0x27] = KEY_RIGHT, [0x28] = KEY_DOWN,
    [0x2c] = KEY_SYSRQ, [0x2d] = KEY_INSERT, [0x2e] = KEY_DELETE, [0x2f] = KEY_HELP,
    [0x30] = KEY_0, [0x31] = KEY_1, [0x32] = KEY_2, [0x33] = KEY_3, [0x34] = KEY_4,
    [0x35] = KEY_5, [0x36] = KEY_6, [0x37] = KEY_7, [0x38] = KEY_8, [0x39] = KEY_9,
    [0x41] = KEY_A, [0x42] = KEY_B, [0x43] = KEY_C, [0x44] = KEY_D, [0x45] = KEY_E,
    [0x46] = KEY_F, [0x47] = KEY_G, [0x48] = KEY_H, [0x49] = KEY_I, [0x4a] = KEY_J,
    [0x4b] = KEY_K, [0x4c] = KEY_L, [0x4d] = KEY_M, [0x4e] = KEY_N, [0x4f] = KEY_O,
    [0x50] = KEY_P, [0x51] = KEY_Q, [0x52] = KEY_R, [0x53] = KEY_S, [0x54] = KEY_T,
    [0x55] = KEY_U, [0x56] = KEY_V, [0x57] = KEY_W, [0x58] = KEY_X, [0x59] = KEY_Y,
    [0x5a] = KEY_Z, [0x5b] = KEY_LEFTMETA, [0x5c] = KEY_RIGHTMETA, [0x5d] = KEY_COMPOSE,
    [0x5f] = KEY_SLEEP,
    [0x60] = KEY_KP0, [0x61] = KEY_KP1, [0x62] = KEY_KP2, [0x63] = KEY_KP3, [0x64] = KEY_KP4,
    [0x65] = KEY_KP5, [0x66] = KEY_KP6, [0x67] = KEY_KP7, [0x68] = KEY_KP8, [0x69] = KEY_KP9,
    [0x6a] = KEY_KPASTERISK, [0x6b] = KEY_KPPLUS, [0x6c] = KEY_KPCOMMA,
    [0x6d] = KEY_KPMINUS, [0x6e] = KEY_KPDOT, [0x6f] = KEY_KPSLASH,
    [0x70] = KEY_F1, [0x71] = KEY_F2, [0x72] = KEY_F3, [0x73] = KEY_F4, [0x74] = KEY_F5,
    [0x75] = KEY_F6, [0x76] = KEY_F7, [0x77] = KEY_F8, [0x78] = KEY_F9, [0x79] = KEY_F10,
    [0x7a] = KEY_F11, [0x7b] = KEY_F12, [0x7c] = KEY_F13, [0x7d] = KEY_F14,
    [0x7e] = KEY_F15, [0x7f] = KEY_F16, [0x80] = KEY_F17, [0x81] = KEY_F18,
    [0x82] = KEY_F19, [0x83] = KEY_F20, [0x84] = KEY_F21, [0x85] = KEY_F22,
    [0x86] = KEY_F23, [0x87] = KEY_F24,
    [0x90] = KEY_NUMLOCK, [0x91] = KEY_SCROLLLOCK,
    [0xa0] = KEY_LEFTSHIFT, [0xa1] = KEY_RIGHTSHIFT, [0xa2] = KEY_LEFTCTRL,
    [0xa3] = KEY_RIGHTCTRL, [0xa4] = KEY_LEFTALT, [0xa5] = KEY_RIGHTALT,
    [0xa6] = KEY_BACK, [0xa7] = KEY_FORWARD, [0xa8] = KEY_REFRESH, [0xa9] = KEY_STOP,
    [0xaa] = KEY_SEARCH, [0xab] = KEY_BOOKMARKS, [0xac] = KEY_HOMEPAGE,
    [0xad] = KEY_MUTE, [0xae] = KEY_VOLUMEDOWN, [0xaf] = KEY_VOLUMEUP,
    [0xb0] = KEY_NEXTSONG, [0xb1] = KEY_PREVIOUSSONG, [0xb2] = KEY_STOPCD,
    [0xb3] = KEY_PLAYPAUSE, [0xb4] = KEY_MAIL, [0xb5] = KEY_MEDIA,
    [0xb6] = KEY_COMPUTER, [0xb7] = KEY_CALC,
    [0xba] = KEY_SEMICOLON, [0xbb] = KEY_EQUAL, [0xbc] = KEY_COMMA, [0xbd] = KEY_MINUS,
    [0xbe] = KEY_DOT, [0xbf] = KEY_SLASH, [0xc0] = KEY_GRAVE, [0xdb] = KEY_LEFTBRACE,
    [0xdc] = KEY_BACKSLASH, [0xdd] = KEY_RIGHTBRACE, [0xde] = KEY_APOSTROPHE,
    [0xe2] = KEY_102ND,
};

int plh_vk_to_evdev(unsigned vk, int extended)
{
    if (vk > 0xff) return 0;
    if (extended) {
        switch (vk) {
        case 0x0d: return KEY_KPENTER;
        case 0x10: return KEY_RIGHTSHIFT;
        case 0x11: return KEY_RIGHTCTRL;
        case 0x12: return KEY_RIGHTALT;
        }
    }
    return vk_tab[vk];
}

static const struct { const char *n; int c; } names[] = {
    {"ESC", KEY_ESC}, {"ENTER", KEY_ENTER}, {"SPACE", KEY_SPACE}, {"TAB", KEY_TAB},
    {"BACKSPACE", KEY_BACKSPACE}, {"LEFTSHIFT", KEY_LEFTSHIFT}, {"RIGHTSHIFT", KEY_RIGHTSHIFT},
    {"LEFTCTRL", KEY_LEFTCTRL}, {"RIGHTCTRL", KEY_RIGHTCTRL}, {"LEFTALT", KEY_LEFTALT},
    {"RIGHTALT", KEY_RIGHTALT}, {"LEFTMETA", KEY_LEFTMETA}, {"CAPSLOCK", KEY_CAPSLOCK},
    {"UP", KEY_UP}, {"DOWN", KEY_DOWN}, {"LEFT", KEY_LEFT}, {"RIGHT", KEY_RIGHT},
    {"F13", KEY_F13}, {"F14", KEY_F14}, {"F15", KEY_F15}, {"F16", KEY_F16},
    {"F17", KEY_F17}, {"F18", KEY_F18}, {"F19", KEY_F19}, {"F20", KEY_F20},
    {"F21", KEY_F21}, {"F22", KEY_F22}, {"F23", KEY_F23}, {"F24", KEY_F24},
};

int plh_evdev_from_name(const char *name)
{
    if (!strncasecmp(name, "KEY_", 4)) name += 4;
    if (name[0] && !name[1] && isalnum((unsigned char)name[0]))
        return plh_vk_to_evdev((unsigned)toupper((unsigned char)name[0]), 0);
    if ((name[0] == 'F' || name[0] == 'f') && name[1] >= '1' && name[1] <= '9') {
        char *end; long n = strtol(name + 1, &end, 10);
        if (*end) n = 0;
        if (n >= 1 && n <= 12) return plh_vk_to_evdev(0x70 + (unsigned)n - 1, 0);
    }
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (!strcasecmp(names[i].n, name)) return names[i].c;
    return 0;
}
