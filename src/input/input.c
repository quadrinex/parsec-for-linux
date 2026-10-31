/* input.c — common facade for the backends. */
#include <errno.h>
#include "plh_input.h"

int plh_input_key_evdev(plh_input *in, int code, int down)
{
    if (!in || code <= 0 || code > 0x2ff) return -EINVAL;
    return in->ops->key(in, code, down);
}

int plh_input_key_scancode(plh_input *in, unsigned scan, int extended, int down)
{
    int c = plh_scancode_to_evdev(scan, extended);
    return c ? plh_input_key_evdev(in, c, down) : -ENOENT;
}

int plh_input_key_vk(plh_input *in, unsigned vk, int extended, int down)
{
    int c = plh_vk_to_evdev(vk, extended);
    return c ? plh_input_key_evdev(in, c, down) : -ENOENT;
}

int plh_input_move_rel(plh_input *in, int dx, int dy)
{
    if (!in) return -EINVAL;
    if (!dx && !dy) return 0;
    return in->ops->move_rel(in, dx, dy);
}

int plh_input_move_abs(plh_input *in, int x, int y)
{
    if (!in) return -EINVAL;
    const plh_rect *d = &in->desktop;
    if (x < d->x) x = d->x;
    if (y < d->y) y = d->y;
    if (x > d->x + d->w - 1) x = d->x + d->w - 1;
    if (y > d->y + d->h - 1) y = d->y + d->h - 1;
    return in->ops->move_abs(in, x, y);
}

int plh_input_button(plh_input *in, enum plh_button b, int down)
{
    if (!in || (int)b < 0 || b >= PLH_BTN_COUNT) return -EINVAL;
    return in->ops->button(in, b, down);
}

int plh_input_wheel(plh_input *in, int v120, int h120)
{
    if (!in) return -EINVAL;
    if (!v120 && !h120) return 0;
    return in->ops->wheel(in, v120, h120);
}

void plh_input_close(plh_input *in)
{
    if (in) in->ops->close(in);
}

/* Same arithmetic as wineserver (queue.c) : x = left + ((nx * width) >> 16). */
void plh_norm65535_to_px(const plh_rect *r, int nx, int ny, int *px, int *py)
{
    *px = r->x + (int)(((long long)nx * r->w) >> 16);
    *py = r->y + (int)(((long long)ny * r->h) >> 16);
}
