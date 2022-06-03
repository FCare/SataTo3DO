#ifndef __USB_HOST_H_INCLUDE__
#define __USB_HOST_H_INCLUDE__

#include "tusb.h"

typedef enum {
  ENUMERATED = 0x1,
  COMMAND_ON_GOING = 0x2,
  DISC_IN = 0x4,
  DISC_MOUNTED = 0x8,
} usb_state_t;

extern uint8_t usb_state;
extern volatile int8_t requestEject;

extern void USB_Host_init();
extern void USB_Host_loop();
extern void USB_reset(void);

extern bool read_complete_cb(uint8_t dev_addr, msc_cbw_t const* cbw, msc_csw_t const* csw);

#endif