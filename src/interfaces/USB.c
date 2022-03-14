#include "USB.h"

#include "bsp/board.h"

void USB_Host_init() {
    board_init();
    tusb_init();
}

void USB_Host_loop()
{
  // tinyusb host task
  tuh_task();

#if CFG_TUH_HID
  hid_app_task();
#endif
}
