#ifndef __USB_HOST_H_INCLUDE__
#define __USB_HOST_H_INCLUDE__

#include "tusb.h"


typedef enum {
  DETACHED = 0x00,
  ATTACHED = 0x01,
  ENUMERATED = 0x02,
  MOUNTED = 0x03,
  EJECTING = 0x04,
  USB_STATE_FLAG = DETACHED|ATTACHED|ENUMERATED|MOUNTED|EJECTING,
  COMMAND_ON_GOING = 0x8,
  UNKNOWN_TYPE = 0x00,
  MSC_TYPE = 0x10,
  CD_TYPE = 0x20,
  PERIPH_TYPE = 0x30,
} usb_state_t;

#define GET_USB_STEP() (usb_state & USB_STATE_FLAG)
#define SET_USB_STEP(A) (usb_state = (usb_state&~USB_STATE_FLAG)|A)
#define GET_USB_PERIPH_TYPE() (usb_state & PERIPH_TYPE)
#define SET_USB_PERIPH_TYPE(A) (usb_state = (usb_state&~PERIPH_TYPE)|A)
#define IS_USB_CMD_ONGOING() ((usb_state & COMMAND_ON_GOING) == COMMAND_ON_GOING)
#define SET_USB_CMD_ONGOING() (usb_state |= COMMAND_ON_GOING)
#define CLEAR_USB_CMD_ONGOING() (usb_state &= ~COMMAND_ON_GOING)

typedef enum {
  DONE = 0x0,
  OPEN,
  READ,
  WRITE,
  CLOSE
} fileCmd_s;



extern uint8_t usb_state;
extern volatile int8_t requestLoad;

extern void USB_Host_init();
extern void USB_Host_loop();
extern void USB_reset(void);

extern bool read_complete_cb(uint8_t dev_addr, msc_cbw_t const* cbw, msc_csw_t const* csw);

#endif