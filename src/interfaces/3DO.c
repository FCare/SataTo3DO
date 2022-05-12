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

#include "write.pio.h"

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
uint sm_write_offset = -1;

uint instr_out;
uint instr_pull;
uint instr_set;
uint instr_jmp;
uint instr_restart;

uint nbWord = 0;

volatile bool interrupt = false;

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

bool getIt() {
  bool hasAnIt = ((pio0->irq & (1<<0)) != 0);
  hw_set_bits(&pio0->irq, (1u << 0)); //clear interrupt
  return hasAnIt;
}

bool shallInterrupt()
{
  if (getIt()) interrupt = true;
  return interrupt;
}

void set3doRaw(uint32_t data, bool cdsten, bool outcdsten, bool cddten, bool outcddten, bool interruptible) {
  while (pio_sm_is_tx_fifo_full(pio0, sm_write) && !(shallInterrupt() && interruptible)) tight_loop_contents();
  if (interrupt && interruptible) {
    // printf("It\n");
    return;
  }
  pio_sm_put(pio0, sm_write, (cdsten << CDSTEN) | (cddten<<CDDTEN) |(data<<CDD0) | (((outcdsten << CDSTEN) | (outcddten<<CDDTEN) |(data<<CDD0))<<16));
  while (pio_sm_is_tx_fifo_full(pio0, sm_write) && !(shallInterrupt() && interruptible)) tight_loop_contents();
  if (interrupt && interruptible) {
    return;
  }
  pio_sm_put(pio0, sm_write, 0x0);
}

void set3doStatus(uint32_t data) {
  getIt();
  set3doRaw(data, 0, 0, 1, 1, false);
}


void set3doData(uint32_t data, bool statusReady, bool continueData) {
  set3doRaw(data, !statusReady, !statusReady, 0, !continueData, true);
}


void initiateData(void) {
  // pio_sm_clear_fifos(pio0, sm_write);
  // while(!pio_sm_is_tx_fifo_empty(pio0, sm_write))
  //   pio_sm_exec(pio0, sm_write, instr_pull);
  pio_sm_set_consecutive_pindirs(pio0, sm_write, CDD0, 8, true);
  // gpio_put(CDDTEN, 0x0);
}

void initiateResponse(CD_request_t request) {
  // pio_sm_clear_fifos(pio0, sm_write);
  // while(!pio_sm_is_tx_fifo_empty(pio0, sm_write))
  //   pio_sm_exec(pio0, sm_write, instr_pull);
  pio_sm_set_consecutive_pindirs(pio0, sm_write, CDD0, 8, true);
  set3doStatus(request);
  // gpio_put(CDSTEN, 0x0);
}

void closeRequestwithStatus() {
  set3doRaw(status, 0, 1, 1, 1, false);
  while (!pio_sm_is_tx_fifo_empty(pio0, sm_write)) tight_loop_contents();
  pio_sm_set_consecutive_pindirs(pio0, sm_write, CDD0, 8, false);
}

void set3doInterruptStatus(uint32_t data) {
  getIt();
  set3doRaw(data, 0, 0, 0, 0, false);
}

void closeRequestwithInterruptedStatus() {
  set3doRaw(status, 0, 1, 0, 0, false);
  while (!pio_sm_is_tx_fifo_empty(pio0, sm_write)) tight_loop_contents();
  pio_sm_set_consecutive_pindirs(pio0, sm_write, CDD0, 8, false);
}

void sendData(int startlba, int nb_block) {
  uint8_t buffer[2500];
  int start = startlba;
  bool statusReady = false;

  if (nb_block == 0) return;
  while (nb_block != 0) {
    readBlock(startlba, 1, &buffer[0]);
    nb_block--;
    startlba++;
    interrupt = false;
    getIt(); //Clear It status
    //Force out as buffer_0
    // while(!pio_sm_is_tx_fifo_empty(pio0, sm_write))
    //   pio_sm_exec(pio0, sm_write, instr_pull);
    // pio_sm_exec(pio0, sm_write, instr_out);
    //Start data
    initiateData();
    for (int i = 0; i<currentDisc.block_size; i++) {
      // if (i < 10) printf("0x%x\n", buffer[i]);
      set3doData(buffer[i], statusReady, true);
      if (interrupt) {
        // pio_sm_set_enabled(pio0, sm_write, false);
    // Need to clear _input shift counter_, as well as FIFO, because there may be
    // partial ISR contents left over from a previous run. sm_restart does this.
        // printf("lvl %d %d\n", pio_sm_get_rx_fifo_level(pio0, sm_write), pio_sm_get_tx_fifo_level(pio0, sm_write) );
        pio_sm_clear_fifos(pio0, sm_write);
        // printf("after lvl %d %d\n", pio_sm_get_rx_fifo_level(pio0, sm_write), pio_sm_get_tx_fifo_level(pio0, sm_write) );
        // pio_sm_restart(pio0, sm_write);
        // printf("pc %d %d\n", pio_sm_get_pc(pio0, sm_write), sm_write_offset );
        pio_sm_exec(pio0, sm_write, instr_jmp);
        // printf("after pc %d %d\n", pio_sm_get_pc(pio0, sm_write), sm_write_offset );
        // pio_sm_set_enabled(pio0, sm_write, true);
        interrupt = false;
        statusReady = false;
        // printf("Send status after interrupt\n");
        //Stack READ_DATA in TX fifo
        //Drain the fifo so that OSR is full of READ_DATA
        // pio_sm_put(pio0, sm_write, (1 << CDSTEN) | (0<<CDDTEN) |(buffer[i]<<CDD0) | (((1 << CDSTEN) | (0<<CDDTEN) |(buffer[i]<<CDD0))<<16));
        // while(!pio_sm_is_tx_fifo_empty(pio0, sm_write))
        //   pio_sm_exec(pio0, sm_write, instr_pull);
        // pio_sm_exec(pio0, sm_write, instr_out);
        // pio_sm_put(pio0, sm_write, 0x0);
        //Push OSR to out
        // printf("PC %x\n", pio_sm_get_pc(pio0, sm_write) - 21);
        // gpio_put(CDDTEN, 0x1);
        set3doInterruptStatus(READ_DATA);
        closeRequestwithInterruptedStatus();
            int val = gpio_get(CDEN);
        absolute_time_t start = get_absolute_time();
        while(absolute_time_diff_us(start,get_absolute_time()) < 590) {
          if (pio_sm_is_tx_fifo_empty(pio0, sm_write)) {
            uint val = buffer[i++];
            uint data = (1 << CDSTEN) | (0<<CDDTEN) |(val<<CDD0) | (((1 << CDSTEN) | (0<<CDDTEN) |(val<<CDD0))<<16);
            pio_sm_put(pio0, sm_write, data);
            pio_sm_put(pio0, sm_write, 0x0);
          }
        }
        // pio_sm_set_enabled(pio0, sm_write, false);
        pio_sm_clear_fifos(pio0, sm_write);
        // pio_sm_restart(pio0, sm_write);
        // printf("pc %d %d\n", pio_sm_get_pc(pio0, sm_write), sm_write_offset );
        uint data = (1<< CDSTEN) | (1<<CDDTEN);
        pio_sm_put(pio0, sm_write, data);
        pio_sm_exec(pio0, sm_write, instr_jmp);
        while (pio_sm_get_pc(pio0, sm_write) != sm_write_offset + 3);
        pio_sm_exec(pio0, sm_write, instr_jmp);
        // pio_sm_set_enabled(pio0, sm_write, true);

        // pio_sm_drain_tx_fifo(pio0, sm_write);
        printf("CDEN %d\n", val);
        return;
      }
      if (i == 34) statusReady = true;;
    }
  }
  set3doStatus(READ_DATA);
  closeRequestwithStatus();
}

void handleCommand(uint32_t data) {
  CD_request_t request = (CD_request_t) GET_BUS(data);
  bool isCmd = (data>>CDCMD)&0x1 == 0x0;
  uint32_t data_in[6];

  // pio0->irq  = 0;

  switch(request) {
    case READ_ID:
    for (int i=0; i<6; i++) {
      data_in[i] = get3doData();
    }
      printf("READ ID\n");
      initiateResponse(READ_ID);
      set3doStatus(0x00); //manufacture Id
      set3doStatus(0x10);
      set3doStatus(0x00); //manufacture number
      set3doStatus(0x01);
      set3doStatus(0x30);
      set3doStatus(0x75);
      set3doStatus(0x00); //revision number
      set3doStatus(0x00);
      set3doStatus(0x00); //flag
      set3doStatus(0x00);
      closeRequestwithStatus();
      break;
    case READ_ERROR:
      for (int i=0; i<6; i++) {
        data_in[i] = get3doData();
      }
      printf("READ ERROR\n");
      initiateResponse(READ_ERROR);
      set3doStatus(0x00);
      set3doStatus(0x00);
      set3doStatus(errorCode); //error code
      set3doStatus(0x00);
      set3doStatus(0x00);
      set3doStatus(0x00);
      set3doStatus(0x00);
      set3doStatus(0x00);
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
      set3doStatus(0xAA); //This means ok
      set3doStatus(0x55); //This means ok. Not the case when no disc
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
      printf("DISC_INFO\n");
      initiateResponse(READ_DISC_INFO);
      // LBA = (((M*60)+S)*75+F)-150
      if (currentDisc.mounted) {
        set3doStatus(currentDisc.format);
        set3doStatus(currentDisc.first_track);
        set3doStatus(currentDisc.last_track);
        set3doStatus(currentDisc.msf[2]);
        set3doStatus(currentDisc.msf[1]);
        set3doStatus(currentDisc.msf[0]);
      } else {
        errorCode = NOT_READY;
        set3doStatus(0x0);
        set3doStatus(0x0);
        set3doStatus(0x0);
        set3doStatus(0x0);
        set3doStatus(0x0);
        set3doStatus(0x0);
      }
      closeRequestwithStatus();
      break;
    case READ_TOC:
      for (int i=0; i<6; i++) {
        data_in[i] = GET_BUS(get3doData());
      }
      printf("READ_TOC %x\n", data_in[2]);
      initiateResponse(READ_TOC);
      set3doStatus(0x0); //NixByte?
      set3doStatus(currentDisc.tracks[data_in[2]].CTRL_ADR); //ADDR
      set3doStatus(currentDisc.tracks[data_in[2]].id); //ENT_NUMBER
      set3doStatus(0x0);//Format
      set3doStatus(currentDisc.tracks[data_in[2]].msf[0]);
      set3doStatus(currentDisc.tracks[data_in[2]].msf[1]);
      set3doStatus(currentDisc.tracks[data_in[2]].msf[2]);
      set3doStatus(0x0);
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
        set3doStatus(0x80);
        set3doStatus(currentDisc.msf[2]); //might some other values like msf for multisession start
        set3doStatus(currentDisc.msf[1]);
        set3doStatus(currentDisc.msf[0]);
        set3doStatus(0x0);
        set3doStatus(0x0);
      } else {
        set3doStatus(0x0);
        set3doStatus(0x0);
        set3doStatus(0x0);
        set3doStatus(0x0);
        set3doStatus(0x0);
        set3doStatus(0x0);
      }
      closeRequestwithStatus();
    break;
    case READ_CAPACITY:
      for (int i=0; i<6; i++) {
        data_in[i] = get3doData();
      }
      printf("READ_CAPACITY\n");
      initiateResponse(READ_CAPACITY);
      set3doStatus(currentDisc.msf[2]);
      set3doStatus(currentDisc.msf[1]);
      set3doStatus(currentDisc.msf[0]);
      set3doStatus(0x0);
      set3doStatus(0x0);
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

  instr_out = pio_encode_out(pio_null, 16);
  instr_pull = pio_encode_pull(pio_null, 32);
  instr_set = pio_encode_set(pio_pins, 0b11);
  instr_jmp = pio_encode_jmp(sm_write_offset+1);
  instr_restart = pio_encode_jmp(sm_write_offset);
  // init_3DO_read_interface();
  // init_3DO_write_interface();
  pio_sm_set_enabled(pio0, sm_write, true);

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
    getIt();
    reset_occured = false;
    data_in = get3doData();
    handleCommand(data_in);
  }
}

// pio0 interrupt handler
void on_pio0_irq() {
  // if (pio_interrupt_get(pio0, 0)) {
  //     // if (!gpio_get(CDDTEN) && !gpio_get(CDSTEN)) interrupt = true;
  //     nbWord++;
  //     // if (!gpio_get(CDDTEN))
  //     // {
  //     //   printf("Drain fifo\n");
  //     //   pio_sm_drain_tx_fifo(pio0, sm_write);
  //     //   // pio_sm_put(pio0, sm_write, 0xFF);
  //     // }
  // }
  // pio_interrupt_clear(pio0, 0);
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
  // gpio_put(CDSTEN, 1);
  gpio_set_dir(CDSTEN, true);
  // gpio_set_pulls(CDSTEN, false, true);

  gpio_init(CDDTEN);
  // gpio_put(CDDTEN, 1);
  gpio_set_dir(CDDTEN, true);
  // gpio_set_pulls(CDDTEN, false, true);


  gpio_init(CDHRD);
  gpio_set_dir(CDHRD, false);

  gpio_init(CDHWR);
  gpio_set_dir(CDHWR, false);

  gpio_set_dir_masked(DATA_BUS_MASK, DATA_BUS_MASK);
  gpio_init_mask(DATA_BUS_MASK);

  pio_set_irq0_source_enabled(pio0, pis_interrupt0, true);
  // irq_set_exclusive_handler(PIO0_IRQ_0, on_pio0_irq);
  // irq_set_enabled(PIO0_IRQ_0, true);

  for (int i = 0; i<32; i++) {
    gpio_set_drive_strength(i, GPIO_DRIVE_STRENGTH_12MA);
  }

#ifndef NO_READ_PIO
  sm_read = 0;
  sm_interrupt = 2;
  offset = pio_add_program(pio0, &read_program);
  read_program_init(pio0, sm_read, offset);
  offset = pio_add_program(pio0, &interrupt_program);
  interrupt_program_init(pio0, sm_interrupt, offset);
#endif

  pio_gpio_init(pio0, CDD0);
  pio_gpio_init(pio0, CDD1);
  pio_gpio_init(pio0, CDD2);
  pio_gpio_init(pio0, CDD3);
  pio_gpio_init(pio0, CDD4);
  pio_gpio_init(pio0, CDD5);
  pio_gpio_init(pio0, CDD6);
  pio_gpio_init(pio0, CDD7);
  pio_gpio_init(pio0, CDSTEN);
  pio_gpio_init(pio0, CDDTEN);
  sm_write = 1;
  sm_write_offset = pio_add_program(pio0, &write_program);
  write_program_init(pio0, sm_write, sm_write_offset);

  printf("wait for msg now\n");


  int clock = clock_get_hz(clk_sys);
  printf("Clock is %d\n", clock);

  multicore_launch_core1(core1_entry);
}
