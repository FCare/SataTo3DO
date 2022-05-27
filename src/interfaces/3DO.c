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

extern bool readBlock(uint32_t start, uint16_t nb_block, uint8_t block_size, uint8_t *buffer);
extern bool readSubQChannel(uint8_t *buffer);
extern bool driveEject(bool eject);
extern bool block_is_ready();
extern bool isAudioBlock(uint32_t start);

#define GET_BUS(A) (((A)>>CDD0)&0xFF)

extern cd_s currentDisc;

typedef enum{
  SPIN_UP = 0x2,
  EJECT_DISC = 0x6,
  INJECT_DISC = 0x7,
  SET_MODE = 0x09,
  READ_DATA = 0x10,
  DATA_PATH_CHECK = 0x80,
  READ_ERROR = 0x82,
  READ_ID    = 0x83,
  READ_CAPACITY = 0x85,
  READ_SUB_Q = 0x87,
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
uint8_t status = DOOR_CLOSED | CHECK_ERROR;

void close_tray(bool close) {
  if (!driveEject(!close)) {
    LOG_SATA("Can not eject/inject\n");
    return;
  }
  status &= ~DOOR_CLOSED;
  if (close) {
    gpio_set_dir(CDRST, true);
    gpio_put(CDRST, 0);
    gpio_put(CDMDCHG, 1); //Under reset
    sleep_ms(200);
    gpio_put(CDRST, 1);
    gpio_set_dir(CDRST, false);
    sleep_ms(150);
    gpio_put(CDMDCHG, 0); //Under reset
    sleep_ms(10);
    gpio_put(CDMDCHG, 1); //Under reset
    sleep_ms(6);
    gpio_put(CDMDCHG, 0); //Under reset
    status |= DOOR_CLOSED | CHECK_ERROR;
    errorCode |= POWER_OR_RESET_OCCURED;
  } else {
    currentDisc.mounted = false;
    status |= CHECK_ERROR;
    errorCode |= DISC_REMOVED;
  }
}

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
}

bool is3doData() {
  return pio_sm_get_rx_fifo_level(pio0, sm_read) != 0;
}

uint32_t get3doData() {
  uint32_t val = 0x0;
  val = pio_sm_get_blocking(pio0, sm_read);
  return val;
}

void print_dma_ctrl(dma_channel_hw_t *channel) {
    uint32_t ctrl = channel->ctrl_trig;
    int rgsz = (ctrl & DMA_CH0_CTRL_TRIG_RING_SIZE_BITS) >> DMA_CH0_CTRL_TRIG_RING_SIZE_LSB;
    LOG_SATA("(%08x) ber %d rer %d wer %d busy %d trq %d cto %d rgsl %d rgsz %d inw %d inr %d sz %d hip %d en %d",
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
  gpio_put(CDSTEN + access, 0x1);
  pio_sm_set_consecutive_pindirs(pio0, sm[access], CDD0, 8, false);
  pio_sm_set_enabled(pio0, sm[access], false);
}

bool sendAnswerStatusMixed(uint8_t *buffer, uint32_t nbWord, uint8_t *buffer_status, uint8_t nbStatus, bool last, bool trace) {
  bool statusStarted = false;
  bool interrupted = false;
  absolute_time_t a,b,c,d,e, s = get_absolute_time();
  bool lastCDEN = gpio_get(CDEN);
  if (trace) a= get_absolute_time();
  restartPio(CHAN_WRITE_DATA);
  if (trace) b= get_absolute_time();
  startDMA(CHAN_WRITE_DATA, buffer, nbWord);
  if (trace) c= get_absolute_time();
  pio_sm_set_consecutive_pindirs(pio0, sm[CHAN_WRITE_DATA], CDD0, 8, true);
  if (trace)  d= get_absolute_time();
  gpio_put(CDDTEN, 0x0);
  if (trace) e= get_absolute_time();
  absolute_time_t start = get_absolute_time();
  bool canBeInterrupted = false;

  if (trace)
    LOG_SATA("a %lld, b %lld, c %lld, d %lld, e %lld\n", absolute_time_diff_us(s,a), absolute_time_diff_us(a,b), absolute_time_diff_us(b,c), absolute_time_diff_us(c,d), absolute_time_diff_us(d,e));

  while (dma_channel_is_busy(CHAN_WRITE_DATA)) {
    if (gpio_get(CDEN) != lastCDEN) {
        lastCDEN = !lastCDEN;
        pio_sm_set_consecutive_pindirs(pio0, sm[CHAN_WRITE_DATA], CDD0, 8, !lastCDEN);
    }
    if((absolute_time_diff_us(start,get_absolute_time()) > 100) && (!statusStarted) && canBeInterrupted && last) {
      statusStarted = true;
      gpio_put(CDSTEN, 0x0);
    }

    if (!gpio_get(CDEN)) {
      start = get_absolute_time();
      canBeInterrupted = true;
    }

    if (!gpio_get(CDHWR))
    {
      pio_sm_set_enabled(pio0, sm[CHAN_WRITE_DATA], false);
      pio_sm_set_consecutive_pindirs(pio0, sm[CHAN_WRITE_DATA], CDD0, 8, false);
      gpio_put(CDDTEN, true);
      return false;
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
  return true;
}
#define NB_BLOCK 1
absolute_time_t lastPacket;
void sendData(int startlba, int nb_block, bool trace) {

  uint8_t buffer[2500*NB_BLOCK];
  uint8_t status_buffer[2] = {READ_DATA, status};
  int start = startlba;
  absolute_time_t a,b,c,d,e, s;
  uint8_t reste = 1;


  if (nb_block == 0) return;
  int id = 0;
  while (nb_block != 0) {
    s = get_absolute_time();
    int current = id;
    int data_idx = 0;
    if (trace) a= get_absolute_time();
    if (!currentDisc.mounted) return;
    readBlock(startlba, NB_BLOCK, currentDisc.block_size_read, &buffer[0]);
    if (trace) b= get_absolute_time();
    while(!block_is_ready());

    if (isAudioBlock(startlba)) {
      if (is_nil_time(lastPacket)) {
        lastPacket = delayed_by_us(get_absolute_time(),13333);
      } else {
        absolute_time_t currentPacket = get_absolute_time();
        int64_t delay = absolute_time_diff_us(currentPacket, lastPacket); /*Right number shall be 1000000/75*/
        if (delay>0) sleep_us(delay);
        else lastPacket = currentPacket;
        lastPacket = delayed_by_us(lastPacket,13333) + reste/3;
        reste = (reste%3)+1;
      }
    }
    if (trace) c = get_absolute_time();
    nb_block--;
    startlba++;
    id = (id++)%2;
    if (trace) d= get_absolute_time();
    if (!sendAnswerStatusMixed(&buffer[0], currentDisc.block_size_read, status_buffer, 2, nb_block == 0, trace)) return;
    if (trace) e = get_absolute_time();
    if (trace)
      LOG_SATA("send data a %lld, b %lld, c %lld, d %lld, e %lld\n", absolute_time_diff_us(s,a), absolute_time_diff_us(a,b), absolute_time_diff_us(b,c), absolute_time_diff_us(c,d), absolute_time_diff_us(d,e));

  }
}

static bool ledState = false;

void handleCommand(uint32_t data) {
  CD_request_t request = (CD_request_t) GET_BUS(data);
  bool isCmd = (data>>CDCMD)&0x1 == 0x0;
  uint32_t data_in[6];
  uint8_t buffer[12];
  uint8_t subBuffer[16];
  uint index = 0;

#ifndef USE_UART_RX
  gpio_set(LED, ledState);
  ledState = !ledState;
#endif

  switch(request) {
    case READ_ID:
    for (int i=0; i<6; i++) {
      data_in[i] = get3doData();
    }
      LOG_SATA("READ ID\n");
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
    case EJECT_DISC:
      for (int i=0; i<6; i++) {
        data_in[i] = get3doData();
      }
      LOG_SATA("EJECT\n");
      close_tray(false);
      buffer[index++] = status;
      sendAnswer(buffer, index, CHAN_WRITE_STATUS);
      break;
    case INJECT_DISC:
        for (int i=0; i<6; i++) {
          data_in[i] = get3doData();
        }
        LOG_SATA("INJECT\n");
        close_tray(true);
        buffer[index++] = status;
        sendAnswer(buffer, index, CHAN_WRITE_STATUS);
        break;
    case READ_ERROR:
      for (int i=0; i<6; i++) {
        data_in[i] = get3doData();
      }
      LOG_SATA("READ ERROR\n");
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
      LOG_SATA("DATA_PATH_CHECK\n");
      buffer[index++] = DATA_PATH_CHECK;
      buffer[index++] = 0xAA; //This means ok
      buffer[index++] = 0x55; //This means ok. Not the case when no disc
      buffer[index++] = status;
      sendAnswer(buffer, index, CHAN_WRITE_STATUS);
      break;
    case SPIN_UP:
      if (currentDisc.mounted) {
        for (int i=0; i<6; i++) {
          data_in[i] = get3doData();
        }
        LOG_SATA("SPIN UP\n");
        buffer[index++] = SPIN_UP;
        if (!(status & DOOR_CLOSED)) {
            status |= CHECK_ERROR;
            errorCode |= ILLEGAL_CMD;
        } else {
          if (!(status & DISK_OK)) {
            status |= CHECK_ERROR;
            errorCode |= DISC_REMOVED;
          } else {
            status |= SPINNING;
          }
        }
        buffer[index++] = status;
        sendAnswer(buffer, index, CHAN_WRITE_STATUS);
      }
      break;
    case READ_DATA:
      if (currentDisc.mounted) {
        for (int i=0; i<6; i++) {
          data_in[i] = GET_BUS(get3doData());
        }
        LOG_SATA("READ DATA MSF %d:%d:%d\n", data_in[0], data_in[1], data_in[2]);
        if (data_in[3] == 0x00) {
          //MSF
          int lba = data_in[0]*60*75+data_in[1]*75+data_in[2] - 150;
          int nb_block = (data_in[4]<<8)|data_in[5];
          sendData(lba, nb_block, false);
        } else {
          //LBA not supported yet
          LOG_SATA("LBA not supported yet\n");
        }
      }
      break;
    case READ_DISC_INFO:
      if (currentDisc.mounted) {
        for (int i=0; i<6; i++) {
          data_in[i] = get3doData();
        }
        LOG_SATA("DISC_INFO\n");
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
      }
      break;
    case READ_TOC:
      if (currentDisc.mounted) {
        for (int i=0; i<6; i++) {
          data_in[i] = GET_BUS(get3doData());
        }
        LOG_SATA("READ_TOC %x\n", data_in[1]);
        buffer[index++] = READ_TOC;
        buffer[index++] = 0x0; //NixByte?
        buffer[index++] = currentDisc.tracks[data_in[1]-1].CTRL_ADR; //ADDR
        buffer[index++] = currentDisc.tracks[data_in[1]-1].id; //ENT_NUMBER
        buffer[index++] = 0x0;//Format
        buffer[index++] = currentDisc.tracks[data_in[1]-1].msf[0];
        buffer[index++] = currentDisc.tracks[data_in[1]-1].msf[1];
        buffer[index++] = currentDisc.tracks[data_in[1]-1].msf[2];
        buffer[index++] = 0x0;
        buffer[index++] = status;
        sendAnswer(buffer, index, CHAN_WRITE_STATUS);
      }
      break;
    case READ_SESSION:
      if (currentDisc.mounted) {
        for (int i=0; i<6; i++) {
          data_in[i] = get3doData();
        }
        LOG_SATA("READ_SESSION\n");
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
      }
    break;
    case READ_CAPACITY:
      if (currentDisc.mounted) {
        for (int i=0; i<6; i++) {
          data_in[i] = get3doData();
        }
        LOG_SATA("READ_CAPACITY\n");
        buffer[index++] = READ_CAPACITY;
        buffer[index++] = currentDisc.msf[0];
        buffer[index++] = currentDisc.msf[1];
        buffer[index++] = currentDisc.msf[2];
        buffer[index++] = 0x0; //0x8?
        buffer[index++] = 0x0;
        buffer[index++] = status;
        sendAnswer(buffer, index, CHAN_WRITE_STATUS);
      }
      break;
      case SET_MODE:
        for (int i=0; i<6; i++) {
          data_in[i] = GET_BUS(get3doData());
        }
        LOG_SATA("SET_MODE %x %x %x %x %x %x\n", data_in[0], data_in[1], data_in[2], data_in[3], data_in[4], data_in[5]);
        /*
        not entirely full
[18:04]
cmd9 has many settings
[18:04]
[0] = 0x9
[18:04]
[1] = setting type
[18:04]
where :
[18:04]
0 = density
[18:04]
1 = error recovery
[18:04]
2 = stop time
[18:05]
3 = speed+pitch
[18:05]
4 = chunk size
[18:06]
density : [2] = density code, [3][4] = block length [5] = flags
[18:06]
error recovery : [2] = type [3] = retry count
[18:07]
stop time : [2] = time in 100ms
[18:07]
speed/pitch: [2]=speed [3][4] = pitch
[18:09]
chunk size : [2] = chunk size (1-8)

fixel — Aujourd’hui à 18:17
on density : [5] flags :
[18:18]
0x80 = 2353 bytes CDDA + error correction (modifié)
[18:19]
0x40 = 2448 bytes CDDA+subcode
[18:19]
0xc0 (0x80|0x40) = 2449 bytes : CDDA+subcode+error correction
[18:19]
0x00 : 2048
[18:21]
what's your reply to 0x83?
*/

        if (data_in[0] == 0x3) {
          if (data_in[1] & (0x80|0x40)) {
            status |= DOUBLE_SPEED;
          } else {
            status &= ~DOUBLE_SPEED;
          }
        }
        if (data_in[0] == 0x0) {
          currentDisc.block_size_read = (data_in[2]<<8)|(data_in[3]);
          if ((data_in[4] & 0xC0) == 0x80) {
            currentDisc.block_size_read = 2353; //CDDA + error correction
          }
          if ((data_in[4] & 0xC0) == 0x40) {
            currentDisc.block_size_read = 2448; //CDDA+subcode
          }
          if ((data_in[4] & 0xC0) == 0xC0) {
            currentDisc.block_size_read = 2449; //CDDA+subcode+error correction
          }
          LOG_SATA("Block size to %d\n",currentDisc.block_size_read );
        }
        buffer[index++] = SET_MODE;
        buffer[index++] = status;
        sendAnswer(buffer, index, CHAN_WRITE_STATUS);
        break;
    case READ_SUB_Q:
    if (currentDisc.mounted) {
        for (int i=0; i<6; i++) {
          data_in[i] = GET_BUS(get3doData());
        }
        LOG_SATA("READ_SUB_Q\n");
        buffer[index++] = READ_SUB_Q;
        readSubQChannel(&subBuffer[0]);
        while(!block_is_ready());

        buffer[index++] = subBuffer[5]; //Ctrl/adr
        buffer[index++] = subBuffer[6]; //trk
        buffer[index++] = subBuffer[7]; //idx
        buffer[index++] = 0;
        buffer[index++] = subBuffer[9]; //total M
        buffer[index++] = subBuffer[10]; //total S
        buffer[index++] = subBuffer[11]; //total F
        buffer[index++] = subBuffer[13]; //current M
        buffer[index++] = subBuffer[14]; //current S
        buffer[index++] = subBuffer[15]; //current F
        buffer[index++] = status;
        sendAnswer(buffer, index, CHAN_WRITE_STATUS);
      }
        break;
    default: LOG_SATA("unknown Cmd %x\n", request);
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
  LOG_SATA("Prog %d offset %d\n", channel, sm_offset[channel]);
}

static absolute_time_t s;
static bool debounceEject = false;

void core1_entry() {

  uint32_t data_in;
  bool reset_occured = true;
  bool ejectState = gpio_get(EJECT);

  lastPacket = nil_time;

  instr_out = pio_encode_out(pio_null, 16);
  instr_pull = pio_encode_pull(pio_null, 32);
  instr_set = pio_encode_set(pio_pins, 0b11);

  for (int i = 0; i< CHAN_MAX; i++) {
    pio_program_init(i);
    setupDMA(i);
  }

  LOG_SATA("Ready\n");
  while (1){
    if (!gpio_get(CDRST)) {
      reset_occured = true;

      wait_out_of_reset();
      errorCode |= POWER_OR_RESET_OCCURED;
      status |= CHECK_ERROR;
    }
    if (gpio_get(CDEN)) {
      LOG_SATA("CD is not enabled\n");
      while(gpio_get(CDEN));
      LOG_SATA("CD is enabled now\n");
    }

    bool ejectCurrent = gpio_get(EJECT);
    if (ejectCurrent != ejectState) {
        if (!debounceEject) s = get_absolute_time();
        debounceEject = true;
        if (absolute_time_diff_us(s, get_absolute_time()) > 2000) {
          ejectState = ejectCurrent;
          //Eject button pressed, toggle tray position
          if (!ejectCurrent) {
            close_tray((status & DOOR_CLOSED) == 0);
          }
          debounceEject = false;
        }
    } else {
        if (debounceEject && (absolute_time_diff_us(s, get_absolute_time()) > 2000)) debounceEject = false;
    }

    reset_occured = false;
    if (is3doData()) {
      data_in = get3doData();
      handleCommand(data_in);
    }
  }
}

void _3DO_init() {
  uint offset = -1;

  gpio_init(CDRST);
  gpio_set_dir(CDRST, false);

  gpio_init(EJECT);
  gpio_set_dir(EJECT, false);

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

  gpio_init(CDDTEN);
  gpio_put(CDDTEN, 1);
  gpio_set_dir(CDDTEN, true);

#ifndef USE_UART_RX
  gpio_init(LED);
  gpio_set_dir(LED, true);
#endif


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

  LOG_SATA("wait for msg now\n");


  int clock = clock_get_hz(clk_sys);
  LOG_SATA("Clock is %d\n", clock);

  multicore_launch_core1(core1_entry);
}
