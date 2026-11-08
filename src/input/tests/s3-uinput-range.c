/* s3-uinput-range.c — spike S3: can an absolute uinput device be recreated live with a larger range, and
 * does KWin register the new device? Emits NO events. Creates "PLH s3 probe" (ABS 0..W-1, 0..H-1), reads its
 * absinfo back through the evdev node, lists KWin's input devices over D-Bus, destroys it.
 *   s3-uinput-range W H [W2 H2]   (second pair = recreate with a different range) */
#include <fcntl.h>
#include <linux/uinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>

static int mk(int w, int h)
{
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) { perror("/dev/uinput"); exit(1); }
    ioctl(fd, UI_SET_EVBIT, EV_KEY); ioctl(fd, UI_SET_KEYBIT, BTN_LEFT);
    ioctl(fd, UI_SET_EVBIT, EV_ABS); ioctl(fd, UI_SET_ABSBIT, ABS_X); ioctl(fd, UI_SET_ABSBIT, ABS_Y);
    struct uinput_setup us = { .id = { BUS_VIRTUAL, 0x1209, 0x7f03, 1 } };
    strcpy(us.name, "PLH s3 probe");
    ioctl(fd, UI_DEV_SETUP, &us);
    struct uinput_abs_setup a = { .code = ABS_X, .absinfo = { .maximum = w - 1 } };
    ioctl(fd, UI_ABS_SETUP, &a);
    a.code = ABS_Y; a.absinfo.maximum = h - 1; ioctl(fd, UI_ABS_SETUP, &a);
    if (ioctl(fd, UI_DEV_CREATE) < 0) { perror("UI_DEV_CREATE"); exit(1); }
    return fd;
}
static void show(int fd)
{
    char sys[64] = "", path[512];
    ioctl(fd, UI_GET_SYSNAME(sizeof sys), sys);
    usleep(500000);
    snprintf(path, sizeof path, "/sys/devices/virtual/input/%s", sys);
    DIR *d = opendir(path); struct dirent *e; char ev[256] = "";
    while (d && (e = readdir(d))) if (!strncmp(e->d_name, "event", 5)) snprintf(ev, sizeof ev, "%s", e->d_name);
    if (d) closedir(d);
    snprintf(path, sizeof path, "/dev/input/%s", ev);
    int efd = open(path, O_RDONLY | O_NONBLOCK);
    if (efd < 0) printf("  %s node=%s: absinfo not readable (event nodes are root:input)\n", sys, ev);
    else {
        struct input_absinfo x, y;
        ioctl(efd, EVIOCGABS(ABS_X), &x); ioctl(efd, EVIOCGABS(ABS_Y), &y);
        printf("  %s node=%s absinfo X 0..%d Y 0..%d\n", sys, ev, x.maximum, y.maximum);
        close(efd);
    }
    char cmd[512];
    snprintf(cmd, sizeof cmd, "busctl --user get-property org.kde.KWin /org/kde/KWin/InputDevice/%s org.kde.KWin.InputDevice name 2>&1 | sed 's/^/  kwin: /'", ev);
    fflush(stdout); system(cmd);
}
int main(int argc, char **argv)
{
    if (argc < 3) return 2;
    int fd = mk(atoi(argv[1]), atoi(argv[2]));
    printf("created %sx%s\n", argv[1], argv[2]); show(fd);
    ioctl(fd, UI_DEV_DESTROY); close(fd);
    if (argc >= 5) {
        usleep(300000);
        fd = mk(atoi(argv[3]), atoi(argv[4]));
        printf("recreated %sx%s\n", argv[3], argv[4]); show(fd);
        ioctl(fd, UI_DEV_DESTROY); close(fd);
    }
    return 0;
}
