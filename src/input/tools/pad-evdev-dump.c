/* pad-evdev-dump — waits for an evdev node named "Microsoft X-Box 360 pad (PLH" , prints its events
 * as "EV type code value" (no SYN lines), "FOUND eventN" when it appears and "REMOVED" when it goes.
 * Read-only; injects nothing. Usage: pad-evdev-dump SECONDS_TO_WAIT_FOR_DEVICE SECONDS_TO_RUN */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

static int find_node(char *out, size_t len)
{
    DIR *d = opendir("/sys/class/input");
    struct dirent *e;
    int r = -1;
    while (d && (e = readdir(d))) {
        char p[300], nm[128] = "";
        if (strncmp(e->d_name, "event", 5)) continue;
        snprintf(p, sizeof p, "/sys/class/input/%s/device/name", e->d_name);
        FILE *f = fopen(p, "r");
        if (!f) continue;
        if (fgets(nm, sizeof nm, f) && !strncmp(nm, "Microsoft X-Box 360 pad (PLH", 27)) { snprintf(out, len, "/dev/input/%s", e->d_name); r = 0; }
        fclose(f);
        if (!r) break;
    }
    if (d) closedir(d);
    return r;
}

int main(int argc, char **argv)
{
    double wait_s = argc > 1 ? atof(argv[1]) : 10, run_s = argc > 2 ? atof(argv[2]) : 10, t0 = now();
    char path[300];
    setvbuf(stdout, NULL, _IOLBF, 0);
    while (find_node(path, sizeof path)) {
        if (now() - t0 > wait_s) { puts("NODEVICE"); return 1; }
        usleep(20000);
    }
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    for (int i = 0; fd < 0 && errno == EACCES && i < 100; i++) { usleep(30000); fd = open(path, O_RDONLY | O_NONBLOCK); }   /* udev adds the uaccess ACL a moment after creation */
    if (fd < 0) { printf("OPENFAIL %s\n", strerror(errno)); return 2; }
    printf("FOUND %s\n", path + 11);
    while (now() - t0 < run_s) {
        struct pollfd pf = { fd, POLLIN, 0 };
        struct input_event ev;
        if (poll(&pf, 1, 100) <= 0) continue;
        ssize_t n = read(fd, &ev, sizeof ev);
        if (n < 0 && errno == ENODEV) { puts("REMOVED"); return 0; }
        if (n != (ssize_t)sizeof ev) { if (pf.revents & (POLLHUP | POLLERR)) { puts("REMOVED"); return 0; } continue; }
        if (ev.type != EV_SYN) printf("EV %d %d %d\n", ev.type, ev.code, ev.value);
    }
    puts("TIMEOUT");
    return 0;
}
