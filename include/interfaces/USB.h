#ifndef __USB_HOST_H_INCLUDE__
#define __USB_HOST_H_INCLUDE__

#include "tusb.h"
#include "diskio.h"


typedef enum {
  DETACHED = 0x00,
  ATTACHED = 0x01,
  ENUMERATED = 0x02,
  CONFIGURED = 0x03,
  MOUNTED = 0x04,
  EJECTING = 0x05,
} usb_state_t;

typedef enum {
  UNKNOWN_TYPE = 0x00,
  MSC_TYPE = 0x1,
  CD_TYPE = 0x2,
} device_type_t;

typedef struct {
  bool mounted;
  uint8_t dev_addr;
  uint8_t lun;
  bool canBeEjected;
  bool canBeLoaded;
  bool isFatFs;
  device_type_t type;
  usb_state_t state;
  bool tray_open;
  FATFS  DiskFATState;
} device_s;


typedef enum {
  DONE = 0x0,
  OPEN,
  READ,
  WRITE,
  CLOSE
} fileCmd_s;

extern uint8_t usb_state;
extern volatile int8_t requestLoad;

extern bool usb_cmd_on_going;

extern void USB_Host_init();
extern void USB_Host_loop();
extern void USB_reset(void);

extern device_s* getDevice(uint8_t dev_addr);
extern device_s* getDeviceIndex(uint8_t idx);

extern bool read_complete_cb(uint8_t dev_addr, msc_cbw_t const* cbw, msc_csw_t const* csw);

#endif