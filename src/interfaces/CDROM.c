#include "USB.h"
#include "3DO.h"
#include "CDFormat.h"
#include "pico/stdio.h"

#if CFG_TUH_MSC
static bool check_eject(uint8_t dev_addr);
static void check_speed(uint8_t dev_addr);
static void check_block(uint8_t dev_addr);
static bool check_subq(uint8_t dev_addr);
#endif

bool CDROM_Host_loop(device_s *dev)
{
#if CFG_TUH_MSC
  if (!usb_cmd_on_going) {
    switch(dev->state) {
      case EJECTING:
        check_eject(dev->dev_addr);
      case MOUNTED:
        check_speed(dev->dev_addr);
        check_block(dev->dev_addr);
        check_subq(dev->dev_addr);
        return true;
        break;
      default:
        return false;
    }
  }
#endif
  return false;
}


#if CFG_TUH_MSC

//--------------------------------------------------------------------+
// MACRO TYPEDEF CONSTANT ENUM DECLARATION
//--------------------------------------------------------------------+

static bool startClose = true;

uint8_t readBuffer[20480];


extern volatile bool is_audio;
extern volatile bool has_subQ;

static bool command_complete_cb(uint8_t dev_addr, msc_cbw_t const* cbw, msc_csw_t const* csw) {
  device_s *dev = getDevice(dev_addr);
  if (csw->status != MSC_CSW_STATUS_GOOD) {
    set3doDriveError();
  }
  dev->tray_open = !dev->tray_open;
  usb_cmd_on_going = false;
}

void wakeUpCDRom(uint8_t dev_addr, uint8_t lun) {
  tuh_msc_test_unit_ready(dev_addr, lun, NULL);
}

bool CDROM_ExecuteEject(uint8_t dev_addr) {
  usb_cmd_on_going = true;
  device_s *dev= getDevice(dev_addr);
  LOG_SATA("Eject CDROM %d\n", dev->tray_open);
  if (!dev->canBeLoaded && !dev->canBeEjected) {
    LOG_SATA("Can not load or eject\n");
    set3doCDReady(dev_addr, false);
    set3doDriveMounted(dev_addr, false);
    return true;
  }

  if (!dev->canBeLoaded && dev->tray_open) {
    LOG_SATA("Can not be loaded - force tray to false\n");
    dev->tray_open = false;
  }
  if (!dev->canBeEjected && !dev->tray_open) {
    LOG_SATA("Can not eject - force tray to true\n");
    dev->tray_open = true;
  }

  if ( !tuh_msc_start_stop(dev->dev_addr, dev->lun, dev->tray_open, true, command_complete_cb)) {
    LOG_SATA("Got error while eject command\n");
  }
  else {
    set3doCDReady(dev_addr, false);
    set3doDriveMounted(dev_addr, false);
    return true;
  }
  return false;
}

static bool check_eject(uint8_t dev_addr) {
  //Execute right now
  device_s *dev= getDevice(dev_addr);
  LOG_SATA("Eject %d\n", dev_addr);
  if (CDROM_ExecuteEject(dev_addr)) dev->state = INQUIRY;
  return true;
}

extern uint32_t start_Block;
extern uint32_t nb_block_Block;
extern uint8_t *buffer_Block;
extern bool blockRequired;
extern bool subqRequired;
extern uint8_t *buffer_subq;

static bool speedChange = false;
static uint16_t CDSpeed;

static void check_block(uint8_t dev_addr) {
  if (blockRequired  && (dev_addr == currentImage.dev_addr)) {
    usb_cmd_on_going = true;
    blockRequired = false;
    if (!is_audio) {
      if ( !tuh_msc_read10_sync(currentImage.dev_addr, currentImage.lun, buffer_Block, start_Block, nb_block_Block, read_complete_cb)) {
        LOG_SATA("Got error with block read\n");
        return;
      }
    } else {
      if ( !tuh_msc_read_cd(currentImage.dev_addr, currentImage.lun, buffer_Block, start_Block, nb_block_Block, has_subQ, read_complete_cb)) {
        LOG_SATA("Got error with block read\n");
        return;
      }
    }
  }
}

static bool check_subq(uint8_t dev_addr) {
  if (subqRequired && (dev_addr == currentImage.dev_addr)) {
    usb_cmd_on_going = true;
    subqRequired = false;
    if (!tuh_msc_read_sub_channel(currentImage.dev_addr, currentImage.lun, buffer_subq, read_complete_cb)) {
      LOG_SATA("Got error with sub Channel read\n");
      return false;
    }
  }
  return true;
}

static void check_speed(uint8_t dev_addr) {
  if (speedChange && (dev_addr == currentImage.dev_addr)) {
    usb_cmd_on_going = true;
    speedChange = false;
    if ( !tuh_msc_set_speed(currentImage.dev_addr, currentImage.lun, CDSpeed, 0xFFFF, read_complete_cb)) {
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
  usb_cmd_on_going = false;
  LOG_SATA("read_header_complete_cb\n");
  device_s *dev = getDevice(dev_addr);
  if (readBuffer[0] == 0x2) {
    /* 00h - audio; 01h - Mode 1 (games) - Mode 2 (CD-XA photoCD) */
    //Photo CD shall have an audio track. CD-i are Mode 2 but without audio.
    dev->rawImage.format = 0x20; //Only CD-ROM, CD-DA and CD-XA are supported
  }
  if (currentImage.dev_addr == 0xFF) {
    memcpy(&currentImage, &dev->rawImage, sizeof(cd_s));
    if (dev->state < MOUNTED) dev->state = MOUNTED;
    set3doCDReady(dev_addr, true);
    set3doDriveMounted(dev_addr, true);
    mediaInterrupt();
  }
  return true;
}

static bool read_toc_complete_cb(uint8_t dev_addr, msc_cbw_t const* cbw, msc_csw_t const* csw) {
  device_s *dev = getDevice(dev_addr);
  dev->rawImage.nb_track = ((readBuffer[0]<<8)+readBuffer[1] - 2)/8;

  dev->rawImage.first_track = readBuffer[2];
  dev->rawImage.last_track = readBuffer[3];

  LOG_SATA("First %d, last %d nb %d\n", dev->rawImage.first_track, dev->rawImage.last_track, dev->rawImage.nb_track);
  for (int i = 0; i < dev->rawImage.nb_track-1; i++) {
    int index = 4+8*i;
    //OxAA as id mean lead out
    dev->rawImage.tracks[i].id = readBuffer[index + 2];
    dev->rawImage.tracks[i].CTRL_ADR = readBuffer[index + 1];
    dev->rawImage.tracks[i].msf[0] = readBuffer[index + 5]; //MSF
    dev->rawImage.tracks[i].msf[1] = readBuffer[index + 6];
    dev->rawImage.tracks[i].msf[2] = readBuffer[index + 7];
    if ((dev->rawImage.tracks[i].CTRL_ADR & 0x4) != 0) dev->rawImage.hasOnlyAudio = false;
    dev->rawImage.tracks[i].lba = dev->rawImage.tracks[i].msf[0]*60*75+dev->rawImage.tracks[i].msf[1]*75+dev->rawImage.tracks[i].msf[2] - 150;
    LOG_SATA("Track[%d] 0x%x (0x%x)=> %d:%d:%d\n", i, dev->rawImage.tracks[i].id, dev->rawImage.tracks[i].CTRL_ADR, dev->rawImage.tracks[i].msf[0], dev->rawImage.tracks[i].msf[1], dev->rawImage.tracks[i].msf[2]);
  }

  uint32_t first_track = dev->rawImage.tracks[0].msf[0]*60*75+dev->rawImage.tracks[0].msf[1]*75+dev->rawImage.tracks[0].msf[2] - 150;
  if (dev->rawImage.tracks[0].CTRL_ADR & 0x4) {
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
  device_s *dev = getDevice(dev_addr);
  dev->rawImage.nb_track = (((readBuffer[0]<<8)+readBuffer[1]) - 2)/8;

  dev->rawImage.first_track = readBuffer[2];
  dev->rawImage.last_track = readBuffer[3];

  LOG_SATA("First %d, last %d nbTrack %d\n", dev->rawImage.first_track, dev->rawImage.last_track, dev->rawImage.nb_track);

  if (dev->rawImage.nb_track > 1) {
    if (!tuh_msc_read_toc(dev_addr, cbw->lun, readBuffer, 1, 0, dev->rawImage.nb_track-1, read_toc_complete_cb)) {
        LOG_SATA("Got error with toc read\n");
        return false;
    }
  } else {
    return read_toc_complete_cb(dev_addr, cbw, csw);
  }
  return true;
}

static uint8_t capabilties_buffer[256];

static bool mode_sense_complete_cb(uint8_t dev_addr, msc_cbw_t const* cbw, msc_csw_t const* csw)
{
  device_s *dev = getDevice(dev_addr);

  dev->canBeLoaded = true;
  dev->canBeEjected = true;

  uint16_t length = (capabilties_buffer[0]<<8)+capabilties_buffer[1];
  uint16_t block_descriptor_length = (capabilties_buffer[6]<<8)+capabilties_buffer[7]+8;
  if ((length < 128) && (block_descriptor_length<length)) {
    if ((capabilties_buffer[block_descriptor_length + 6] >> 5) == 0) dev->canBeLoaded = false;
    if ((capabilties_buffer[block_descriptor_length + 6] & 0x8) == 0) dev->canBeEjected = false;
    LOG_SATA("Can Load %d, Can Eject %d %d\n", dev->canBeLoaded, dev->canBeEjected);
    dev->state = CONFIGURED;
    return true;
  }
}

static bool CheckCDCapabilities(uint8_t dev_addr) {
  device_s *dev = getDevice(dev_addr);

  return tuh_msc_mode_sense(dev_addr, dev->lun, 0x2A, 0x0, 0x0, 128, &capabilties_buffer[0], mode_sense_complete_cb);
}

void CDROM_ready(uint8_t dev_addr, bool ready) {
  //Get capabilities in sync
  device_s *dev = getDevice(dev_addr);
  if (dev->state == ENUMERATED) {
    CheckCDCapabilities(dev_addr);
  }
  if (!ready) return;
}

bool CDROM_Inquiry(uint8_t dev_addr, uint8_t lun) {

  device_s *dev = getDevice(dev_addr);
  if (tuh_msc_get_block_size(dev_addr, lun) == 0) {
    return false;
  }
  dev->tray_open = false; //In case of slot-in consider it has started and tray is closed
  dev->useable = true;


  LOG_SATA("Use the CD as potential source\n");
  dev->rawImage.dev_addr = dev->dev_addr;
  dev->rawImage.lun = dev->lun;
  // Get capacity of device
  dev->rawImage.hasOnlyAudio = true;
  dev->rawImage.nb_block = tuh_msc_get_block_count(dev_addr, lun);
  dev->rawImage.block_size = tuh_msc_get_block_size(dev_addr, lun);
  dev->rawImage.block_size_read = dev->rawImage.block_size;

  int lba = dev->rawImage.nb_block + 150;
  dev->rawImage.msf[0] = lba/(60*75);
  lba %= 60*75;
  dev->rawImage.msf[1] = lba / 75;
  dev->rawImage.msf[2] = lba % 75;

  //Assume type is CD-DA or CD-ROM always
  dev->rawImage.format = 0x0; /*00 CD-DA or CD-ROM / 10 CD-I / 20 XA */

  LOG_SATA("Disk Size: %lu MB\r\n", dev->rawImage.nb_block / ((1024*1024)/dev->rawImage.block_size));
  LOG_SATA("Block Count = %lu, Block Size: %lu\r\n", dev->rawImage.nb_block, dev->rawImage.block_size);
  LOG_SATA("Disc duration is %02d:%02d:%02d\n", dev->rawImage.msf[0], dev->rawImage.msf[1], dev->rawImage.msf[2]);

  if (!tuh_msc_read_toc(dev_addr, lun, readBuffer, 1, 0, 0, read_toc_light_complete_cb)) {
    LOG_SATA("Got error with toc read\n");
    return false;
  }

  setCDSpeed(706); //4x speed

  return true;
}

#endif
