#include "USB.h"
#include "3DO.h"
#include "CDFormat.h"
#include "pico/stdio.h"

#if CFG_TUH_MSC
static bool check_eject();
static void check_speed();
static void check_block();
static bool check_subq();
#endif

bool CDROM_Host_loop()
{
  // tinyusb host task
#if CFG_TUH_MSC
  if(usb_state & ENUMERATED) {
    if (!(usb_state & COMMAND_ON_GOING)) {
      if (!check_eject()) {
        if (usb_state & DISC_MOUNTED) {
          check_speed();
          check_block();
          check_subq();
          return true;
        } else {
          return false;
        }
      }
    }
  }
  return true;
#endif
}


#if CFG_TUH_MSC

//--------------------------------------------------------------------+
// MACRO TYPEDEF CONSTANT ENUM DECLARATION
//--------------------------------------------------------------------+

volatile bool inquiry_cb_flag;

static bool startClose = true;

uint8_t readBuffer[20480];


extern volatile bool is_audio;
extern volatile bool has_subQ;

static bool command_complete_cb(uint8_t dev_addr, msc_cbw_t const* cbw, msc_csw_t const* csw) {
  if ((csw->status != MSC_CSW_STATUS_PASSED) && (currentDisc.mounted)) {
    set3doDriveError();
  }
  usb_state &= ~COMMAND_ON_GOING;
}

bool CDROM_ExecuteEject(bool eject) {
  usb_state |= COMMAND_ON_GOING;
  printf("ExecuteEject %d\n", eject);
  if ( !tuh_msc_start_stop(currentDisc.dev_addr, currentDisc.lun, eject, true, command_complete_cb)) {
    LOG_SATA("Got error while eject command\n");
  }
  else {
    if (!eject) {
      set3doCDReady(false);
      set3doDriveMounted(false);
      currentDisc.mounted = false;
      usb_state &= ~DISC_MOUNTED;
    }
    return true;
  }
  return false;
}

static bool check_eject() {
  if (requestEject!=-1) {
    //Execute right now
    LOG_SATA("Eject %d\n", requestEject);
    if (CDROM_ExecuteEject(requestEject != 0))
      requestEject = -1;
    return true;
  }
  return false;
}

extern uint32_t start_Block;
extern uint32_t nb_block_Block;
extern uint8_t *buffer_Block;
extern bool blockRequired;
extern bool subqRequired;
extern uint8_t *buffer_subq;

static bool speedChange = false;
static uint16_t CDSpeed;

static void check_block() {
  if (blockRequired) {
    usb_state |= COMMAND_ON_GOING;
    blockRequired = false;
    if (!is_audio) {
      if ( !tuh_msc_read10(currentDisc.dev_addr, currentDisc.lun, buffer_Block, start_Block, nb_block_Block, read_complete_cb)) {
        LOG_SATA("Got error with block read\n");
        return;
      }
    } else {
      if ( !tuh_msc_read_cd(currentDisc.dev_addr, currentDisc.lun, buffer_Block, start_Block, nb_block_Block, has_subQ, read_complete_cb)) {
        LOG_SATA("Got error with block read\n");
        return;
      }
    }
  }
}

static bool check_subq() {
  if (subqRequired) {
    usb_state |= COMMAND_ON_GOING;
    subqRequired = false;
    if (!tuh_msc_read_sub_channel(currentDisc.dev_addr, currentDisc.lun, buffer_subq, read_complete_cb)) {
      LOG_SATA("Got error with sub Channel read\n");
      return false;
    }
  }
  return true;
}

static void check_speed() {
  if (speedChange) {
    usb_state |= COMMAND_ON_GOING;
    speedChange = false;
    if ( !tuh_msc_set_speed(currentDisc.dev_addr, currentDisc.lun, CDSpeed, 0xFFFF, read_complete_cb)) {
      LOG_SATA("Got error with block read\n");
      return;
    }
  }
}

static bool setCDSpeed(uint16_t speed) {
  CDSpeed = speed;
  speedChange = true;
  return true;
}

static bool read_header_complete_cb(uint8_t dev_addr, msc_cbw_t const* cbw, msc_csw_t const* csw) {
  usb_state &= ~COMMAND_ON_GOING;
  printf("format %x\n", readBuffer[0]);
  if (readBuffer[0] == 0x2) {
    /* 00h - audio; 01h - Mode 1 (games) - Mode 2 (CD-XA photoCD) */
    //Photo CD shall have an audio track. CD-i are Mode 2 but without audio.
    currentDisc.format = 0x20; //Only CD-ROM, CD-DA and CD-XA are supported
  }
  currentDisc.mounted = true;
  usb_state |= DISC_MOUNTED;
  set3doCDReady(true);
  set3doDriveMounted(true);
  return true;
}

static bool read_toc_complete_cb(uint8_t dev_addr, msc_cbw_t const* cbw, msc_csw_t const* csw) {
  currentDisc.nb_track = ((readBuffer[0]<<8)+readBuffer[1] - 2)/8;

  currentDisc.first_track = readBuffer[2];
  currentDisc.last_track = readBuffer[3];

  LOG_SATA("First %d, last %d nb %d\n", currentDisc.first_track, currentDisc.last_track, currentDisc.nb_track);
  for (int i = 0; i < currentDisc.nb_track-1; i++) {
    int index = 4+8*i;
    //OxAA as id mean lead out
    currentDisc.tracks[i].id = readBuffer[index + 2];
    currentDisc.tracks[i].CTRL_ADR = readBuffer[index + 1];
    currentDisc.tracks[i].msf[0] = readBuffer[index + 5]; //MSF
    currentDisc.tracks[i].msf[1] = readBuffer[index + 6];
    currentDisc.tracks[i].msf[2] = readBuffer[index + 7];
    if ((currentDisc.tracks[i].CTRL_ADR & 0x4) != 0) currentDisc.hasOnlyAudio = false;
    currentDisc.tracks[i].lba = currentDisc.tracks[i].msf[0]*60*75+currentDisc.tracks[i].msf[1]*75+currentDisc.tracks[i].msf[2] - 150;
    LOG_SATA("Track[%d] 0x%x (0x%x)=> %d:%d:%d\n", i, currentDisc.tracks[i].id, currentDisc.tracks[i].CTRL_ADR, currentDisc.tracks[i].msf[0], currentDisc.tracks[i].msf[1], currentDisc.tracks[i].msf[2]);
  }

  uint32_t first_track = currentDisc.tracks[0].msf[0]*60*75+currentDisc.tracks[0].msf[1]*75+currentDisc.tracks[0].msf[2] - 150;
  if (currentDisc.tracks[0].CTRL_ADR & 0x4) {
    if (!tuh_msc_read_header(dev_addr, cbw->lun, readBuffer, first_track, read_header_complete_cb)) {
      LOG_SATA("Got error with header read\n");
      return false;
    }
  } else {
    readBuffer[0] = 0x0;
    read_header_complete_cb(dev_addr, cbw, csw);
  }
  return true;
}

static bool read_toc_light_complete_cb(uint8_t dev_addr, msc_cbw_t const* cbw, msc_csw_t const* csw) {
  currentDisc.nb_track = (((readBuffer[0]<<8)+readBuffer[1]) - 2)/8;

  currentDisc.first_track = readBuffer[2];
  currentDisc.last_track = readBuffer[3];

  LOG_SATA("First %d, last %d nbTrack %d\n", currentDisc.first_track, currentDisc.last_track, currentDisc.nb_track);

  if (currentDisc.nb_track > 1) {
    if (!tuh_msc_read_toc(dev_addr, cbw->lun, readBuffer, 1, 0, currentDisc.nb_track-1, read_toc_complete_cb)) {
        LOG_SATA("Got error with toc read\n");
        return false;
    }
  } else {
    return read_toc_complete_cb(dev_addr, cbw, csw);
  }
  return true;
}

bool CDROM_Inquiry(uint8_t dev_addr, msc_cbw_t const* cbw, msc_csw_t const* csw) {

  // Get capacity of device
  currentDisc.hasOnlyAudio = true;
  currentDisc.nb_block = tuh_msc_get_block_count(dev_addr, cbw->lun);
  currentDisc.block_size = tuh_msc_get_block_size(dev_addr, cbw->lun);
  currentDisc.block_size_read = currentDisc.block_size;

  int lba = currentDisc.nb_block + 150;
  currentDisc.msf[0] = lba/(60*75);
  lba %= 60*75;
  currentDisc.msf[1] = lba / 75;
  currentDisc.msf[2] = lba % 75;

  //Assume type is CD-DA or CD-ROM always
  currentDisc.format = 0x0; /*00 CD-DA or CD-ROM / 10 CD-I / 20 XA */

  LOG_SATA("Disk Size: %lu MB\r\n", currentDisc.nb_block / ((1024*1024)/currentDisc.block_size));
  LOG_SATA("Block Count = %lu, Block Size: %lu\r\n", currentDisc.nb_block, currentDisc.block_size);
  LOG_SATA("Disc duration is %2d:%2d:%2d\n", currentDisc.msf[0], currentDisc.msf[1], currentDisc.msf[2]);

  if (!tuh_msc_read_toc(dev_addr, cbw->lun, readBuffer, 1, 0, 0, read_toc_light_complete_cb)) {
      LOG_SATA("Got error with toc read\n");
      return false;
  }

  setCDSpeed(706); //4x speed
  return true;
}

#endif
