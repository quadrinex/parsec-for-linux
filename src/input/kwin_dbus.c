/* kwin_dbus.c — KWin settings over session DBus for our virtual devices.
 * "flat" acceleration profile (factor 1) on the virtual relative mouse: without it, libinput
 * applies adaptive acceleration and a Parsec delta of +10 becomes +3..+9 (measured).
 * KWin persists the setting in ~/.config/kcminputrc, in a per-device group (ROLLBACK #4). */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <systemd/sd-bus.h>
#include "plh_input.h"

static int has_sysname(sd_bus *bus, const char *ev)
{
    sd_bus_error e = SD_BUS_ERROR_NULL;
    char **names = NULL;
    int found = 0;
    if (sd_bus_get_property_strv(bus, "org.kde.KWin", "/org/kde/KWin/InputDevice",
                                 "org.kde.KWin.InputDeviceManager", "devicesSysNames", &e, &names) >= 0) {
        for (char **p = names; p && *p; p++) { if (!strcmp(*p, ev)) found = 1; free(*p); }
        free(names);
    }
    sd_bus_error_free(&e);
    return found;
}

int plh_kwin_set_flat_accel(const char *evnode, int timeout_ms, char *err, size_t errlen)
{
    sd_bus *bus = NULL;
    sd_bus_error e = SD_BUS_ERROR_NULL;
    char path[128];
    int r = sd_bus_open_user(&bus);
    if (r < 0) { snprintf(err, errlen, "bus session: %s", strerror(-r)); return r; }
    for (int t = 0; !has_sysname(bus, evnode); t += 50) {
        if (t >= timeout_ms) { snprintf(err, errlen, "KWin does not see %s", evnode); sd_bus_unref(bus); return -ETIMEDOUT; }
        struct timespec ts = { 0, 50 * 1000000L }; nanosleep(&ts, NULL);
    }
    snprintf(path, sizeof path, "/org/kde/KWin/InputDevice/%s", evnode);
    r = sd_bus_set_property(bus, "org.kde.KWin", path, "org.kde.KWin.InputDevice",
                            "pointerAccelerationProfileFlat", &e, "b", 1);
    if (r >= 0)
        r = sd_bus_set_property(bus, "org.kde.KWin", path, "org.kde.KWin.InputDevice",
                                "pointerAcceleration", &e, "d", 0.0);
    if (r < 0) snprintf(err, errlen, "%s: %s", path, e.message ? e.message : strerror(-r));
    sd_bus_error_free(&e);
    sd_bus_unref(bus);
    return r < 0 ? r : 0;
}
