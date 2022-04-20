#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "pico/multicore.h"

#include "3DO.h"

#ifndef NO_READ_PIO
#include "read.pio.h"
#endif

#ifndef NO_WRITE_PIO
#include "write.pio.h"
#endif

typedef enum{
  READ_ERROR = 0x82,
  READ_ID    = 0x83,
}CD_request_t;

uint sm_read = -1;
uint sm_write = -1;

void wait_out_of_reset() {
  while( !gpio_get(CDRST)) {
    gpio_put(CDMDCHG, 1); //Under reset
  }
  sleep_ms(150);
  gpio_put(CDMDCHG, 0); //Under reset
  sleep_ms(10);
  gpio_put(CDMDCHG, 1); //Under reset
  sleep_ms(6);
  gpio_put(CDMDCHG, 0); //Under reset
}

uint32_t get3doData() {
  uint32_t val = 0x0;
#ifdef NO_READ_PIO
  while(gpio_get(CDHWR) != 0);
  val = gpio_get_all();
  while(gpio_get(CDHWR) != 1);
#else
  val = pio_sm_get_blocking(pio0, sm_read);
#endif
  return val;
}

void set3doData(uint32_t data) {
#ifdef NO_WRITE_PIO
  gpio_put_masked(0xFF<<CDD0, (data&0xFF)<<CDD0);
  while(gpio_get(CDHRD) != 0);
  while(gpio_get(CDHRD) != 1);
#else
  pio_sm_put_blocking(pio0, sm_write, data<<CDD0);
  pio_sm_get_blocking(pio0, sm_write);
#endif
}

void outSync() {
#ifndef NO_WRITE_PIO
  while(!pio_sm_is_tx_fifo_empty (pio0, sm_write));
#endif
}

void handleCommand(uint32_t data) {
  CD_request_t request = (CD_request_t) (data>>CDD0)&0xFF;
  bool isCmd = (data>>CDCMD)&0x1 == 0x0;
  uint32_t data_in[6];
  switch(request) {
    case READ_ID:
    for (int i=0; i<6; i++) {
      data_in[i] = get3doData();
    }
      printf("Got All packets\n");
      gpio_set_dir_masked(0xFF<<CDD0, 0xFF<<CDD0);
      pio_sm_set_consecutive_pindirs(pio0, sm_write, CDD0, 8, true);
      gpio_put(CDSTEN, 0x0);
      // pio_restart_sm_mask(pio0, 1<< sm_write);
      set3doData(READ_ID);
      set3doData(0x00); //manufacture Id
      set3doData(0x10);
      set3doData(0x00); //manufacture number
      set3doData(0x01);
      set3doData(0x00);
      set3doData(0x00);
      set3doData(0x00); //revision number
      set3doData(0x00);
      set3doData(0x00); //flag
      set3doData(0x00);
      set3doData(0x0B); //status

      // set_3D0_interface_write(false);
      pio_sm_set_consecutive_pindirs(pio0, sm_write, CDD0, 8, false);
      // gpio_set_dir_masked(0xFF<<CDD0, 0x0);
      gpio_put(CDSTEN, 0x1);

      // set_3D0_interface_read(true);
      printf("All done\n");
      break;
    case READ_ERROR:
      for (int i=0; i<6; i++) {
        data_in[i] = get3doData();
        printf("La %x\n", (data_in[i]>>CDD0) & 0xFF);
      }
      break;
    default: printf("unknown Cmd %x\n", request);
  }
}

void core1_entry() {

  uint32_t data_in;
  bool reset_occured = true;
  // init_3DO_read_interface();
  // init_3DO_write_interface();

  printf("Ready\n");
  while (1){
    if (!gpio_get(CDRST)) {
      reset_occured = true;

      // set_3D0_interface_read(false);
      // set_3D0_interface_write(false);
      wait_out_of_reset();
    }
    if (gpio_get(CDEN)) {
      printf("CD is not enabled\n");
      while(gpio_get(CDEN)); //Attendre d'avoir le EN
      printf("CD is enabled now\n");
    }
    reset_occured = false;
    data_in = get3doData();
    handleCommand(data_in);
  }
}

void _3DO_init() {
  uint offset = -1;
  // gpio_init_mask(DATA_MASK);
  // gpio_init_mask(CTRL_MASK);
  // gpio_init_mask(OUTPUT_MASK);
  // gpio_set_dir_out_masked(OUTPUT_MASK);
  //
  // gpio_put(CDSTEN, 1); //CDSTEN is 1 when requested status is available and not read
  // gpio_put(CDDTEN, 1); //CDDTEN is 1 when requested data is available and not read


  gpio_init(CDRST);
  gpio_set_dir(CDRST, false);

  gpio_init(CDWAIT);
  gpio_put(CDWAIT, 1);  //CDWAIT is always 1
  gpio_set_dir(CDWAIT, true);

  gpio_init(CDEN);
  gpio_set_dir(CDEN, false);

  gpio_init(CDMDCHG);
  gpio_put(CDMDCHG, 1);
  gpio_set_dir(CDMDCHG, true);


  gpio_init(CDSTEN);
  gpio_put(CDSTEN, 1);
  gpio_set_dir(CDSTEN, true);
  gpio_set_pulls(CDSTEN, false, true);

  gpio_init(CDDTEN);
  gpio_put(CDDTEN, 1);
  gpio_set_dir(CDDTEN, true);
  gpio_set_pulls(CDDTEN, false, true);


  gpio_init(CDHRD);
  gpio_set_dir(CDHRD, false);

  gpio_init(CDHWR);
  gpio_set_dir(CDHWR, false);

  gpio_set_dir_masked(0xFF<<CDD0, 0x0);
  gpio_init_mask(0xFF<<CDD0);

#ifndef NO_READ_PIO
  sm_read = 0;
  offset = pio_add_program(pio0, &read_program);
  read_program_init(pio0, sm_read, offset);
#endif

#ifndef NO_WRITE_PIO
  pio_gpio_init(pio0, CDD0);
  pio_gpio_init(pio0, CDD1);
  pio_gpio_init(pio0, CDD2);
  pio_gpio_init(pio0, CDD3);
  pio_gpio_init(pio0, CDD4);
  pio_gpio_init(pio0, CDD5);
  pio_gpio_init(pio0, CDD6);
  pio_gpio_init(pio0, CDD7);
  sm_write = 1;
  offset = pio_add_program(pio0, &write_program);
  write_program_init(pio0, sm_write, offset);
#endif

  printf("wait for msg now\n");


  int clock = clock_get_hz(clk_sys);
  printf("Clock is %d\n", clock);

  multicore_launch_core1(core1_entry);
}
