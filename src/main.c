#include "USB.h" //USB Setup

#include <stdio.h>
#include "pico/stdlib.h"

int main(void)
{
	USB_Host_init();
	TU_LOG1("Sata to 3DO\r\n");

	while(1) {
		// LCD_loop();
		USB_Host_loop();
	}

	// LCD_0in96_deinit();
  return 0;
}
