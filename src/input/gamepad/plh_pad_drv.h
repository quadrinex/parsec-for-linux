/* plh_pad_drv.h — private interface between plh-vigem.sys (kernel side, runs in Wine's winedevice)
 * and plh-pad-relay.exe (user side). Shared by both; no ViGEmBus code is copied here: the bus
 * protocol constants are in plh-vigem.c. */
#ifndef PLH_PAD_DRV_H
#define PLH_PAD_DRV_H
#include <stdint.h>

#define PLH_PAD_SLOTS 4
/* The driver's device interface (same GUID as the public ViGEmBus bus interface). */
#define PLH_PAD_LINK_NAME   "\\\\.\\PlhPad"          /* private symlink used by the relay */

/* IOCTL_PLH_PAD_GET_EVENTS: output = array of plh_pad_evt, returns what is queued (maybe nothing). */
#define IOCTL_PLH_PAD_GET_EVENTS 0x00222400u      /* CTL_CODE(FILE_DEVICE_UNKNOWN, 0x900, BUFFERED, ANY) */

enum plh_pad_evt_type { PLH_PAD_EVT_PLUG = 1, PLH_PAD_EVT_UNPLUG = 2, PLH_PAD_EVT_REPORT = 3 };

struct plh_pad_evt {            /* 20 bytes */
    uint32_t type;              /* enum plh_pad_evt_type */
    uint32_t slot;              /* 0..PLH_PAD_SLOTS-1 */
    uint16_t buttons;           /* XUSB/XINPUT_GAMEPAD button bits */
    uint8_t  lt, rt;
    int16_t  lx, ly, rx, ry;
};
#endif
