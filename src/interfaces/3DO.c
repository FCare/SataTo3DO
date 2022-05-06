#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "pico/multicore.h"

#include "3DO.h"

#define DATA_BUS_MASK (0xFF<<CDD0)

#ifndef NO_READ_PIO
#include "read.pio.h"
#include "interrupt.pio.h"
#endif

#ifndef NO_WRITE_PIO
#include "write.pio.h"
#endif

extern bool readBlock(uint32_t start, uint16_t nb_block, uint8_t *buffer);

#define GET_BUS(A) (((A)>>CDD0)&0xFF)

extern cd_s currentDisc;

typedef enum{
  SPIN_UP = 0x2,
  SET_MODE = 0x09,
  READ_DATA = 0x10,
  DATA_PATH_CHECK = 0x80,
  READ_ERROR = 0x82,
  READ_ID    = 0x83,
  READ_CAPACITY = 0x85,
  READ_DISC_INFO = 0x8B,
  READ_TOC = 0x8C,
  READ_SESSION = 0x8D,
}CD_request_t;

typedef enum{
  DOOR_CLOSED =	0x80,
  CADDY_IN = 0x40,
  SPINNING = 0x20,
  CHECK_ERROR = 0x10,
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
uint sm_interrupt = -1;
uint sm_write = -1;

bool interrupt = false;
bool sendingData = false;

uint8_t errorCode = POWER_OR_RESET_OCCURED;
uint8_t status = DOOR_CLOSED;

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

void set3doCDReady(bool on) {
  if (on) status |= DISK_OK;
  else status &= ~DISK_OK;
}

void set3doDriveMounted(bool on) {
  if (on) {
    status |= CADDY_IN | SPINNING;
  } else {
    status &= ~CADDY_IN & ~SPINNING;
  }
}

void set3doDriveReady() {
  status |= DOOR_CLOSED; //Stat drive is always close on start
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
  while (!pio_sm_is_tx_fifo_empty(pio0, sm_write));
  pio_sm_put(pio0, sm_write, data<<CDD0);
  while (pio_sm_is_rx_fifo_empty(pio0, sm_write));
  pio_sm_get(pio0, sm_write);
#endif
}

void set3doDataInterruptible(uint32_t data) {
#ifdef NO_WRITE_PIO
  gpio_put_masked(0xFF<<CDD0, (data&0xFF)<<CDD0);
  while(gpio_get(CDHRD) != 0);
  while(gpio_get(CDHRD) != 1);
#else
  while (pio_sm_is_tx_fifo_full(pio0, sm_write)) if (interrupt) return;
  pio_sm_put(pio0, sm_write, data<<CDD0);
  while (pio_sm_is_rx_fifo_empty(pio0, sm_write)) if (interrupt) return;
  pio_sm_get(pio0, sm_write);
#endif
}

void initiateData(void) {
#ifdef NO_WRITE_PIO
  gpio_set_dir_out_masked(DATA_BUS_MASK);
#else
  pio_sm_set_consecutive_pindirs(pio0, sm_write, CDD0, 8, true);
#endif
  gpio_put(CDDTEN, 0x0);
}

void closeData(void) {
  gpio_put(CDDTEN, 0x1);
}

void initiateResponse(CD_request_t request) {
#ifdef NO_WRITE_PIO
  gpio_set_dir_out_masked(DATA_BUS_MASK);
#else
  pio_sm_set_consecutive_pindirs(pio0, sm_write, CDD0, 8, true);
#endif
  gpio_put(CDSTEN, 0x0);
  set3doData(request);
}

void closeRequestwithStatus() {
#ifdef NO_WRITE_PIO
  gpio_put_masked(0xFF<<CDD0, (data&0xFF)<<CDD0);
  while(gpio_get(CDHRD) != 0);
  while(gpio_get(CDHRD) != 1);
  gpio_set_dir_in_masked(DATA_BUS_MASK);
#else
  while (!pio_sm_is_tx_fifo_empty(pio0, sm_write));
  pio_sm_put(pio0, sm_write, status<<CDD0);
  while (pio_sm_is_rx_fifo_empty(pio0, sm_write));
  gpio_put(CDSTEN, 0x1);
  pio_sm_get(pio0, sm_write);
  pio_sm_set_consecutive_pindirs(pio0, sm_write, CDD0, 8, false);
#endif
}

void sendData(int startlba, int nb_block) {
  uint8_t buffer[2500];
  int start = startlba;

  if (nb_block == 0) return;
  sendingData = true;
  while (nb_block != 0) {
    readBlock(startlba, 1, &buffer[0]);
    nb_block--;
    startlba++;
    for (int i = 0; i<currentDisc.block_size; i++) {
      if (i == 0) initiateData();
      // if (i < 10) printf("0x%x\n", buffer[i]);
      interrupt = false;
      set3doDataInterruptible(buffer[i]);
      if (interrupt && sendingData) {
        printf("Send status after interrupt\n");
        sendingData = false;
        set3doData(READ_DATA);
        closeRequestwithStatus();
      }
      if (i == 100) gpio_put(CDSTEN, 0x0);
    }
    closeData();
  }
  if (sendingData) {
    set3doData(READ_DATA);
    closeRequestwithStatus();
  }
  sendingData = false;
}

void handleCommand(uint32_t data) {
  CD_request_t request = (CD_request_t) GET_BUS(data);
  bool isCmd = (data>>CDCMD)&0x1 == 0x0;
  uint32_t data_in[6];
  switch(request) {
    case READ_ID:
    for (int i=0; i<6; i++) {
      data_in[i] = get3doData();
    }
      printf("READ ID\n");
      initiateResponse(READ_ID);
      set3doData(0x00); //manufacture Id
      set3doData(0x10);
      set3doData(0x00); //manufacture number
      set3doData(0x01);
      set3doData(0x30);
      set3doData(0x75);
      set3doData(0x00); //revision number
      set3doData(0x00);
      set3doData(0x00); //flag
      set3doData(0x00);
      closeRequestwithStatus();
      break;
    case READ_ERROR:
      for (int i=0; i<6; i++) {
        data_in[i] = get3doData();
      }
      printf("READ ERROR\n");
      initiateResponse(READ_ERROR);
      set3doData(0x00);
      set3doData(0x00);
      set3doData(errorCode); //error code
      set3doData(0x00);
      set3doData(0x00);
      set3doData(0x00);
      set3doData(0x00);
      set3doData(0x00);
      status &= ~CHECK_ERROR;
      errorCode = NO_ERROR;
      closeRequestwithStatus();
      break;
    case DATA_PATH_CHECK:
      for (int i=0; i<6; i++) {
        data_in[i] = get3doData();
      }
      printf("DATA_PATH_CHECK\n");
      initiateResponse(DATA_PATH_CHECK);
      set3doData(0xAA); //This means ok
      set3doData(0x55); //This means ok. Not the case when no disc
      closeRequestwithStatus();
      break;
    case SPIN_UP:
      for (int i=0; i<6; i++) {
        data_in[i] = get3doData();
      }
      printf("SPIN UP\n");
      initiateResponse(SPIN_UP);
      if (!(status & DISK_OK)) {
        status |= CHECK_ERROR;
        errorCode |= DISC_REMOVED;
      } else {
        status |= SPINNING;
      }
      closeRequestwithStatus();
      break;
    case READ_DATA:
      for (int i=0; i<6; i++) {
        data_in[i] = GET_BUS(get3doData());
      }
      printf("READ DATA MSF %d:%d:%d\n", data_in[0], data_in[1], data_in[2]);
      if (data_in[3] == 0x00) {
        //MSF
        int lba = data_in[0]*60*75+data_in[1]*75+data_in[2] - 150;
        int nb_block = (data_in[4]<<8)|data_in[5];
        sendData(lba, nb_block);
      } else {
        //LBA not supported yet
        printf("LBA not supported yet\n");
      }
      break;
    case READ_DISC_INFO:
      for (int i=0; i<6; i++) {
        data_in[i] = get3doData();
      }
      printf("READ_DISC_INFO\n");
      initiateResponse(READ_DISC_INFO);
      // LBA = (((M*60)+S)*75+F)-150
      if (currentDisc.mounted) {
        set3doData(currentDisc.format);
        set3doData(currentDisc.first_track);
        set3doData(currentDisc.last_track);
        set3doData(currentDisc.msf[2]);
        set3doData(currentDisc.msf[1]);
        set3doData(currentDisc.msf[0]);
      } else {
        errorCode = NOT_READY;
        set3doData(0x0);
        set3doData(0x0);
        set3doData(0x0);
        set3doData(0x0);
        set3doData(0x0);
        set3doData(0x0);
      }
      closeRequestwithStatus();
      break;
    case READ_TOC:
      for (int i=0; i<6; i++) {
        data_in[i] = GET_BUS(get3doData());
      }
      printf("READ_TOC %x\n", data_in[2]);
      initiateResponse(READ_TOC);
      set3doData(0x0); //NixByte?
      set3doData(currentDisc.tracks[data_in[2]].CTRL_ADR); //ADDR
      set3doData(currentDisc.tracks[data_in[2]].id); //ENT_NUMBER
      set3doData(0x0);//Format
      set3doData(currentDisc.tracks[data_in[2]].msf[0]);
      set3doData(currentDisc.tracks[data_in[2]].msf[1]);
      set3doData(currentDisc.tracks[data_in[2]].msf[2]);
      set3doData(0x0);
      closeRequestwithStatus();
      break;
    case READ_SESSION:
      for (int i=0; i<6; i++) {
        data_in[i] = get3doData();
      }
      printf("READ_SESSION\n");
      initiateResponse(READ_SESSION);
      if (currentDisc.multiSession) {
        //TBD with a multisession disc
        set3doData(0x80);
        set3doData(currentDisc.msf[2]); //might some other values like msf for multisession start
        set3doData(currentDisc.msf[1]);
        set3doData(currentDisc.msf[0]);
        set3doData(0x0);
        set3doData(0x0);
      } else {
        set3doData(0x0);
        set3doData(0x0);
        set3doData(0x0);
        set3doData(0x0);
        set3doData(0x0);
        set3doData(0x0);
      }
      closeRequestwithStatus();
    break;
    case READ_CAPACITY:
      for (int i=0; i<6; i++) {
        data_in[i] = get3doData();
      }
      printf("READ_CAPACITY\n");
      initiateResponse(READ_CAPACITY);
      set3doData(currentDisc.msf[2]);
      set3doData(currentDisc.msf[1]);
      set3doData(currentDisc.msf[0]);
      set3doData(0x0);
      set3doData(0x0);
      closeRequestwithStatus();
      break;
      case SET_MODE:
        for (int i=0; i<6; i++) {
          data_in[i] = GET_BUS(get3doData());
        }
        if (data_in[0] == 0x3) {
          if (data_in[1] & (0x80|0x40))
            status |= DOUBLE_SPEED;
          else
            status &= ~DOUBLE_SPEED;
        }
        printf("SET_MODE\n");
        initiateResponse(SET_MODE);
        closeRequestwithStatus();
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

// pio0 interrupt handler
void on_pio0_irq() {
  if (pio_interrupt_get(pio0, 0)) {
      interrupt = true;
      printf("CDEN\n");
      if (!gpio_get(CDDTEN))
      {
        printf("Drain fifo\n");
        pio_sm_drain_tx_fifo(pio0, sm_write);
        // pio_sm_put(pio0, sm_write, 0xFF);
      }
  }
  pio_interrupt_clear(pio0, 0);
  irq_clear(PIO0_IRQ_0);
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

  gpio_set_dir_masked(DATA_BUS_MASK, DATA_BUS_MASK);
  gpio_init_mask(DATA_BUS_MASK);

  pio_set_irq0_source_enabled(pio0, pis_interrupt0, true);
  irq_set_exclusive_handler(PIO0_IRQ_0, on_pio0_irq);
  irq_set_enabled(PIO0_IRQ_0, true);

#ifndef NO_READ_PIO
  sm_read = 0;
  sm_interrupt = 2;
  offset = pio_add_program(pio0, &read_program);
  read_program_init(pio0, sm_read, offset);
  offset = pio_add_program(pio0, &interrupt_program);
  interrupt_program_init(pio0, sm_interrupt, offset);
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
