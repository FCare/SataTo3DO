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
  SPIN_UP = 0x2,
  DATA_PATH_CHECK = 0x80,
  READ_ERROR = 0x82,
  READ_ID    = 0x83,
  READ_DISC_INFO = 0x8B,
}CD_request_t;

typedef enum{
  DOOR_CLOSED =	0x80,
  CADDY_IN = 0x40,
  SPINNING = 0x20,
  CHECK = 0x10,
  BUSY_NEW = 0x8,
  DOOR_LOCKED = 0x04,
  DOUBLE_SPEED = 0x02,
  DISK_OK = 0x01
}CD_status_t;

typedef enum{
  NO_ERROR = 0x0,
  SOFT_READ_RETRY = 0x1,
  SOFT_READ_CORRECTION = 0x2,
  NOT_READY = 0x3,
  NO_TOC = 0x4,
  HARD_READ = 0x5,
  SEEK_FAIL = 0x6,
  TRACKING_FAIL = 0x7,
  DRIVE_RAM_ERROR = 0x8,
  SELF_TEST_ERROR = 0x9,
  FOCUSING_FAIL = 0xA,
  SPINDLE_FAIL = 0xB,
  DATA_PATH_FAIL = 0xC,
  ILLEGAL_LBA = 0xD,
  ILLEGAL_CDB = 0xE,
  END_USER_TRACK = 0xF,
  ILLEGAL_MODE = 0x10,
  MEDIA_CHANGED = 0x11,
  POWER_OR_RESET_OCCURED = 0x12,
  DRIVE_ROM_FAIL = 0x13,
  ILLEGAL_CMD = 0x14,
  DISC_REMOVED = 0x15,
  HARDWARE_ERROR = 0x16,
  ILLEGAL_REQUEST = 0x17,
}CD_error_t;

uint sm_read = -1;
uint sm_write = -1;

uint8_t errorCode = POWER_OR_RESET_OCCURED;
uint8_t status = 0x0; //DOOR_CLOSED;

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

void set3doDriveReady(bool on) {
  if (on) status |= DISK_OK;
  else status &= ~DISK_OK;
}

void set3doDriveMounted(bool on) {
  if (on) {
    status |= DOOR_CLOSED| CADDY_IN | SPINNING;
  } else {
    status &= ~CADDY_IN & ~SPINNING;
  }
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

void handleCommand(uint32_t data) {
  CD_request_t request = (CD_request_t) (data>>CDD0)&0xFF;
  bool isCmd = (data>>CDCMD)&0x1 == 0x0;
  uint32_t data_in[6];
  switch(request) {
    case READ_ID:
    for (int i=0; i<6; i++) {
      data_in[i] = get3doData();
    }
      printf("READ ID\n");
      pio_sm_set_consecutive_pindirs(pio0, sm_write, CDD0, 8, true);
      gpio_put(CDSTEN, 0x0);
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
      set3doData(status); //status 0xB
      gpio_put(CDSTEN, 0x1);
      pio_sm_set_consecutive_pindirs(pio0, sm_write, CDD0, 8, false);

      // set_3D0_interface_read(true);
      printf("All done\n");
      break;
    case READ_ERROR:
      for (int i=0; i<6; i++) {
        data_in[i] = get3doData();
      }
      printf("READ ERROR\n");
      pio_sm_set_consecutive_pindirs(pio0, sm_write, CDD0, 8, true);
      gpio_put(CDSTEN, 0x0);
      set3doData(READ_ERROR);
      set3doData(0x00);
      set3doData(0x00);
      set3doData(errorCode); //error code
      set3doData(0x00);
      set3doData(0x00);
      set3doData(0x00);
      set3doData(0x00);
      set3doData(0x00);
      set3doData(status); //status 0x80
      gpio_put(CDSTEN, 0x1);
      pio_sm_set_consecutive_pindirs(pio0, sm_write, CDD0, 8, false);
      errorCode = NO_ERROR;
      break;
    case DATA_PATH_CHECK:
      for (int i=0; i<6; i++) {
        data_in[i] = get3doData();
      }
      printf("DATA_PATH_CHECK\n");
      pio_sm_set_consecutive_pindirs(pio0, sm_write, CDD0, 8, true);
      gpio_put(CDSTEN, 0x0);
      set3doData(DATA_PATH_CHECK);
      set3doData(0xAA);
      set3doData(0x55);
      set3doData(status); //status 0x80
      gpio_put(CDSTEN, 0x1);
      pio_sm_set_consecutive_pindirs(pio0, sm_write, CDD0, 8, false);
      break;
    case SPIN_UP:
      for (int i=0; i<6; i++) {
        data_in[i] = get3doData();
      }
      printf("SPIN UP\n");
      pio_sm_set_consecutive_pindirs(pio0, sm_write, CDD0, 8, true);
      gpio_put(CDSTEN, 0x0);
      set3doData(SPIN_UP);
      set3doData(status); //status 0x80
      gpio_put(CDSTEN, 0x1);
      pio_sm_set_consecutive_pindirs(pio0, sm_write, CDD0, 8, false);
      break;
    case READ_DISC_INFO:
      for (int i=0; i<6; i++) {
        data_in[i] = get3doData();
      }
      printf("READ_DISC_INFO\n");
      pio_sm_set_consecutive_pindirs(pio0, sm_write, CDD0, 8, true);
      gpio_put(CDSTEN, 0x0);
      set3doData(READ_DISC_INFO);
      set3doData(status); //status 0x80
      gpio_put(CDSTEN, 0x1);
      pio_sm_set_consecutive_pindirs(pio0, sm_write, CDD0, 8, false);
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

  gpio_set_dir_masked(0xFF<<CDD0, 0xFF<<CDD0);
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
