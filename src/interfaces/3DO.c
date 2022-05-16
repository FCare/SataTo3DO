#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/dma.h"
#include "hardware/structs/bus_ctrl.h"
#include "pico/multicore.h"

#include "3DO.h"

#define DATA_BUS_MASK (0xFF<<CDD0)

#include "read.pio.h"

#include "write.pio.h"

extern bool readBlock(uint32_t start, uint16_t nb_block, uint8_t *buffer);
extern bool block_is_ready();

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

typedef enum {
  CHAN_WRITE_STATUS = 0,
  CHAN_WRITE_DATA,
  CHAN_MAX
} DMA_chan_t;


dma_channel_config config[CHAN_MAX];
uint8_t sm[CHAN_MAX];
uint8_t sm_offset[CHAN_MAX];
int channel[CHAN_MAX];


uint sm_read = -1;

uint instr_out;
uint instr_pull;
uint instr_set;
uint instr_jmp[CHAN_MAX];
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
  val = pio_sm_get_blocking(pio0, sm_read);
  return val;
}

void print_dma_ctrl(dma_channel_hw_t *channel) {
    uint32_t ctrl = channel->ctrl_trig;
    int rgsz = (ctrl & DMA_CH0_CTRL_TRIG_RING_SIZE_BITS) >> DMA_CH0_CTRL_TRIG_RING_SIZE_LSB;
    printf("(%08x) ber %d rer %d wer %d busy %d trq %d cto %d rgsl %d rgsz %d inw %d inr %d sz %d hip %d en %d",
           (uint) ctrl,
           ctrl & DMA_CH0_CTRL_TRIG_AHB_ERROR_BITS ? 1 : 0,
           ctrl & DMA_CH0_CTRL_TRIG_READ_ERROR_BITS ? 1 : 0,
           ctrl & DMA_CH0_CTRL_TRIG_WRITE_ERROR_BITS ? 1 : 0,
           ctrl & DMA_CH0_CTRL_TRIG_BUSY_BITS ? 1 : 0,
           (int) ((ctrl & DMA_CH0_CTRL_TRIG_TREQ_SEL_BITS) >> DMA_CH0_CTRL_TRIG_TREQ_SEL_LSB),
           (int) ((ctrl & DMA_CH0_CTRL_TRIG_CHAIN_TO_BITS) >> DMA_CH0_CTRL_TRIG_CHAIN_TO_LSB),
           ctrl & DMA_CH0_CTRL_TRIG_RING_SEL_BITS ? 1 : 0,
           rgsz ? (1 << rgsz) : 0,
           ctrl & DMA_CH0_CTRL_TRIG_INCR_WRITE_BITS ? 1 : 0,
           ctrl & DMA_CH0_CTRL_TRIG_INCR_READ_BITS ? 1 : 0,
           1 << ((ctrl & DMA_CH0_CTRL_TRIG_DATA_SIZE_BITS) >> DMA_CH0_CTRL_TRIG_DATA_SIZE_LSB),
           ctrl & DMA_CH0_CTRL_TRIG_HIGH_PRIORITY_BITS ? 1 : 0,
           ctrl & DMA_CH0_CTRL_TRIG_EN_BITS ? 1 : 0);
}

void restartPio(uint8_t channel) {
  pio_sm_drain_tx_fifo(pio0, sm[channel]);
  pio_sm_restart(pio0, sm[channel]);
  pio_sm_exec(pio0, sm[channel], instr_jmp[channel]);
  pio_sm_set_enabled(pio0, sm[channel], true);
}

void startDMA(uint8_t access, uint8_t *buffer, uint32_t nbWord) {
  dma_channel_transfer_from_buffer_now(channel[access], buffer, nbWord);
}

void sendAnswer(uint8_t *buffer, uint32_t nbWord, uint8_t access) {
  restartPio(access);
  startDMA(access, buffer, nbWord);
  pio_sm_set_consecutive_pindirs(pio0, sm[access], CDD0, 8, true);
  gpio_put(CDSTEN + access, 0x0);

  dma_channel_wait_for_finish_blocking(access);
  while(!pio_sm_is_tx_fifo_empty(pio0, sm[access]));
  while (pio_sm_get_pc(pio0, sm[access]) != sm_offset[access]);
  // while (pio_sm_get_pc(pio0, sm[access]) != sm_offset[access]) printf("Pc %d\n",pio_sm_get_pc(pio0, sm[access]));
  // while (pio_sm_get_pc(pio0, sm[access]) != sm_offset[access]) printf("Pc %d\n",pio_sm_get_pc(pio0, sm[access]));
  gpio_put(CDSTEN + access, 0x1);
  pio_sm_set_consecutive_pindirs(pio0, sm[access], CDD0, 8, false);
  pio_sm_set_enabled(pio0, sm[access], false);
}

void sendAnswerStatusMixed(uint8_t *buffer, uint32_t nbWord, uint8_t *buffer_status, uint8_t nbStatus, bool last) {
  bool statusStarted = false;
  bool interrupted = false;
  bool lastCDEN = gpio_get(CDEN);
  restartPio(CHAN_WRITE_DATA);
  startDMA(CHAN_WRITE_DATA, buffer, nbWord);
  pio_sm_set_consecutive_pindirs(pio0, sm[CHAN_WRITE_DATA], CDD0, 8, true);
  gpio_put(CDDTEN, 0x0);
  absolute_time_t start = get_absolute_time();

  while (dma_channel_is_busy(CHAN_WRITE_DATA)) {
    if (gpio_get(CDEN) != lastCDEN) {
        lastCDEN = !lastCDEN;
        pio_sm_set_consecutive_pindirs(pio0, sm[CHAN_WRITE_DATA], CDD0, 8, !lastCDEN);
    }
    if((absolute_time_diff_us(start,get_absolute_time()) > 100) && (!statusStarted) && last) {
      statusStarted = true;
      gpio_put(CDSTEN, 0x0);
    }
    if (gpio_get(CDEN) && statusStarted) {
      interrupted = true;
      pio_sm_set_enabled(pio0, sm[CHAN_WRITE_DATA], false);
      sendAnswer(buffer_status, nbStatus, CHAN_WRITE_STATUS);
      pio_sm_set_consecutive_pindirs(pio0, sm[CHAN_WRITE_DATA], CDD0, 8, true);
      pio_sm_set_enabled(pio0, sm[CHAN_WRITE_DATA], true);
    }
  }
  while(!pio_sm_is_tx_fifo_empty(pio0, sm[CHAN_WRITE_DATA]));
  while (pio_sm_get_pc(pio0, sm[CHAN_WRITE_DATA]) != sm_offset[CHAN_WRITE_DATA]);
  gpio_put(CDDTEN, 0x1);
  if (!interrupted && last) sendAnswer(buffer_status, nbStatus, CHAN_WRITE_STATUS);
  else pio_sm_set_consecutive_pindirs(pio0, sm[CHAN_WRITE_DATA], CDD0, 8, false);
  pio_sm_set_enabled(pio0, sm[CHAN_WRITE_DATA], false);
}

void sendData(int startlba, int nb_block) {

  uint8_t buffer[2500];
  uint8_t status_buffer[2] = {READ_DATA, status};
  int start = startlba;

  if (nb_block == 0) return;
  uint8_t *buffer_out[2];
  int id = 0;
  buffer_out[0] = &buffer[0];
  buffer_out[1] = &buffer[currentDisc.block_size];
  readBlock(startlba, 1, buffer_out[0]);
  while (nb_block != 0) {
    int current = id;
    while(!block_is_ready());
    nb_block--;
    startlba++;
    id = (id++)%2;
    if (nb_block != 0) readBlock(startlba, 1, buffer_out[id]);
    sendAnswerStatusMixed(buffer_out[current], currentDisc.block_size, status_buffer, 2, nb_block == 0);
  }
}

void handleCommand(uint32_t data) {
  CD_request_t request = (CD_request_t) GET_BUS(data);
  bool isCmd = (data>>CDCMD)&0x1 == 0x0;
  uint32_t data_in[6];
  uint8_t buffer[12];
  uint index = 0;

  // pio0->irq  = 0;

  switch(request) {
    case READ_ID:
    for (int i=0; i<6; i++) {
      data_in[i] = get3doData();
    }
      printf("READ ID\n");
      buffer[index++] = READ_ID;
      buffer[index++] =0x00; //manufacture Id
      buffer[index++] =0x10;
      buffer[index++] =0x00; //manufacture number
      buffer[index++] =0x01;
      buffer[index++] =0x30;
      buffer[index++] =0x75;
      buffer[index++] =0x00; //revision number
      buffer[index++] =0x00;
      buffer[index++] =0x00; //flag
      buffer[index++] =0x00;
      buffer[index++] = status;
      sendAnswer(buffer, index, CHAN_WRITE_STATUS);
      break;
    case READ_ERROR:
      for (int i=0; i<6; i++) {
        data_in[i] = get3doData();
      }
      printf("READ ERROR\n");
      buffer[index++] = READ_ERROR;
      buffer[index++] = 0x00;
      buffer[index++] = 0x00;
      buffer[index++] = errorCode; //error code
      buffer[index++] = 0x00;
      buffer[index++] = 0x00;
      buffer[index++] = 0x00;
      buffer[index++] = 0x00;
      buffer[index++] = 0x00;
      status &= ~CHECK_ERROR;
      errorCode = NO_ERROR;
      buffer[index++] = status;
      sendAnswer(buffer, index, CHAN_WRITE_STATUS);
      break;
    case DATA_PATH_CHECK:
      for (int i=0; i<6; i++) {
        data_in[i] = get3doData();
      }
      printf("DATA_PATH_CHECK\n");
      buffer[index++] = DATA_PATH_CHECK;
      buffer[index++] = 0xAA; //This means ok
      buffer[index++] = 0x55; //This means ok. Not the case when no disc
      buffer[index++] = status;
      sendAnswer(buffer, index, CHAN_WRITE_STATUS);
      break;
    case SPIN_UP:
      for (int i=0; i<6; i++) {
        data_in[i] = get3doData();
      }
      printf("SPIN UP\n");
      buffer[index++] = SPIN_UP;
      if (!(status & DISK_OK)) {
        status |= CHECK_ERROR;
        errorCode |= DISC_REMOVED;
      } else {
        status |= SPINNING;
      }
      buffer[index++] = status;
      sendAnswer(buffer, index, CHAN_WRITE_STATUS);
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
      buffer[index++] = READ_DISC_INFO;
      // LBA = (((M*60)+S)*75+F)-150
      if (currentDisc.mounted) {
        buffer[index++] = currentDisc.format;
        buffer[index++] = currentDisc.first_track;
        buffer[index++] = currentDisc.last_track;
        buffer[index++] = currentDisc.msf[0];
        buffer[index++] = currentDisc.msf[1];
        buffer[index++] = currentDisc.msf[2];
      } else {
        errorCode = NOT_READY;
        buffer[index++] = 0x0;
        buffer[index++] = 0x0;
        buffer[index++] = 0x0;
        buffer[index++] = 0x0;
        buffer[index++] = 0x0;
        buffer[index++] = 0x0;
      }
      buffer[index++] = status;
      sendAnswer(buffer, index, CHAN_WRITE_STATUS);
      break;
    case READ_TOC:
      for (int i=0; i<6; i++) {
        data_in[i] = GET_BUS(get3doData());
      }
      printf("READ_TOC %x\n", data_in[2]);
      buffer[index++] = READ_TOC;
      buffer[index++] = 0x0; //NixByte?
      buffer[index++] = currentDisc.tracks[data_in[2]].CTRL_ADR; //ADDR
      buffer[index++] = currentDisc.tracks[data_in[2]].id; //ENT_NUMBER
      buffer[index++] = 0x0;//Format
      buffer[index++] = currentDisc.tracks[data_in[2]].msf[0];
      buffer[index++] = currentDisc.tracks[data_in[2]].msf[1];
      buffer[index++] = currentDisc.tracks[data_in[2]].msf[2];
      buffer[index++] = 0x0;
      buffer[index++] = status;
      sendAnswer(buffer, index, CHAN_WRITE_STATUS);
      break;
    case READ_SESSION:
      for (int i=0; i<6; i++) {
        data_in[i] = get3doData();
      }
      printf("READ_SESSION\n");
      buffer[index++] = READ_SESSION;
      if (currentDisc.multiSession) {
        //TBD with a multisession disc
        buffer[index++] = 0x80;
        buffer[index++] = currentDisc.msf[0]; //might some other values like msf for multisession start
        buffer[index++] = currentDisc.msf[1];
        buffer[index++] = currentDisc.msf[2];
        buffer[index++] = 0x0;
        buffer[index++] = 0x0;
      } else {
        buffer[index++] = 0x0;
        buffer[index++] = 0x0;
        buffer[index++] = 0x0;
        buffer[index++] = 0x0;
        buffer[index++] = 0x0;
        buffer[index++] = 0x0;
      }
      buffer[index++] = status;
      sendAnswer(buffer, index, CHAN_WRITE_STATUS);
    break;
    case READ_CAPACITY:
      for (int i=0; i<6; i++) {
        data_in[i] = get3doData();
      }
      printf("READ_CAPACITY\n");
      buffer[index++] = READ_CAPACITY;
      buffer[index++] = currentDisc.msf[0];
      buffer[index++] = currentDisc.msf[1];
      buffer[index++] = currentDisc.msf[2];
      buffer[index++] = 0x0;
      buffer[index++] = 0x0;
      buffer[index++] = status;
      sendAnswer(buffer, index, CHAN_WRITE_STATUS);
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
        buffer[index++] = SET_MODE;
        buffer[index++] = status;
        sendAnswer(buffer, index, CHAN_WRITE_STATUS);
        break;
    default: printf("unknown Cmd %x\n", request);
  }
}

void setupDMA(uint8_t access) {
  channel[access] = dma_claim_unused_channel(true);
  config[access] = dma_channel_get_default_config(channel[access]);
  channel_config_set_transfer_data_size(&config[access], DMA_SIZE_8);
  channel_config_set_read_increment(&config[access], true);
  channel_config_set_write_increment(&config[access], false);
  channel_config_set_irq_quiet(&config[access], true);
  channel_config_set_dreq(&config[access], DREQ_PIO0_TX0 + access);
  dma_channel_set_write_addr(channel[access], &pio0->txf[sm[access]], false);
  dma_channel_set_config(channel[access], &config[access], false);

  bus_ctrl_hw->priority = BUSCTRL_BUS_PRIORITY_DMA_W_BITS | BUSCTRL_BUS_PRIORITY_DMA_R_BITS;
}

void pio_program_init(int channel) {
  sm[channel] = channel;
  sm_offset[channel] = pio_add_program(pio0, &write_program);
  write_program_init(pio0, sm[channel], sm_offset[channel]);
  instr_jmp[channel] = pio_encode_jmp(sm_offset[channel]);
  printf("Prog %d offset %d\n", channel, sm_offset[channel]);
}

void core1_entry() {

  uint32_t data_in;
  bool reset_occured = true;

  instr_out = pio_encode_out(pio_null, 16);
  instr_pull = pio_encode_pull(pio_null, 32);
  instr_set = pio_encode_set(pio_pins, 0b11);
  // instr_restart = pio_encode_jmp(sm_write_offset);
  // init_3DO_read_interface();
  // init_3DO_write_interface();

  for (int i = 0; i< CHAN_MAX; i++) {
    pio_program_init(i);
    setupDMA(i);
  }


  // pio_sm_set_enabled(pio0, sm_write, true);

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
  // gpio_set_pulls(CDSTEN, false, true);

  gpio_init(CDDTEN);
  gpio_put(CDDTEN, 1);
  gpio_set_dir(CDDTEN, true);
  // gpio_set_pulls(CDDTEN, false, true);


  gpio_init(CDHRD);
  gpio_set_dir(CDHRD, false);

  gpio_init(CDHWR);
  gpio_set_dir(CDHWR, false);

  gpio_set_dir_masked(DATA_BUS_MASK, DATA_BUS_MASK);
  gpio_init_mask(DATA_BUS_MASK);

  for (int i = 0; i<32; i++) {
    gpio_set_drive_strength(i, GPIO_DRIVE_STRENGTH_12MA);
  }

  sm_read = CHAN_MAX;
  offset = pio_add_program(pio0, &read_program);
  read_program_init(pio0, sm_read, offset);

  pio_gpio_init(pio0, CDD0);
  pio_gpio_init(pio0, CDD1);
  pio_gpio_init(pio0, CDD2);
  pio_gpio_init(pio0, CDD3);
  pio_gpio_init(pio0, CDD4);
  pio_gpio_init(pio0, CDD5);
  pio_gpio_init(pio0, CDD6);
  pio_gpio_init(pio0, CDD7);

  printf("wait for msg now\n");


  int clock = clock_get_hz(clk_sys);
  printf("Clock is %d\n", clock);

  multicore_launch_core1(core1_entry);
}
