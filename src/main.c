#include "USB.h" //USB Setup
#include "3DO.h"

#include "bsp/board.h"
#include <stdio.h>
#include "pico/stdlib.h"

int main(void)
{
	board_init();
	sleep_ms(100);
	_3DO_init();
	sleep_ms(500);
	USB_Host_init();
	LOG_SATA("SLOCODE 3DO\r\n");

	while(1) {
		// LCD_loop();
		USB_Host_loop();
	}

	// LCD_0in96_deinit();
  return 0;
}
