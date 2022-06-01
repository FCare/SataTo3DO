#ifndef __USB_HOST_H_INCLUDE__
#define __USB_HOST_H_INCLUDE__

#include "tusb.h"

extern void USB_Host_init();

extern void USB_Host_loop();

extern void USB_reset(void);

extern void hid_app_task(void);

#endif