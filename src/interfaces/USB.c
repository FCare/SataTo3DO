#include "USB.h"
#include "3DO.h"
#include "pico/stdio.h"

#if CFG_TUH_MSC
static bool check_eject();
static void check_block();
static void check_mount();
#endif

typedef enum {
  ENUMERATED = 0x1,
  COMMAND_ON_GOING = 0x2,
  DISC_IN = 0x4,
  DISC_MOUNTED = 0x8,
} usb_state_t;


uint8_t usb_state = 0;

void USB_Host_init() {
    stdio_init_all();
    tusb_init();
}

void USB_Host_loop()
{
  // tinyusb host task
  tuh_task();
#if CFG_TUH_MSC
  if(usb_state & ENUMERATED) {
    if (!(usb_state & COMMAND_ON_GOING)) {
      if (!check_eject()) {
        if (usb_state & DISC_MOUNTED) {
          check_block();
        } else {
          check_mount();
        }
      }
    }
  }
#endif
}


#if CFG_TUH_MSC

//--------------------------------------------------------------------+
// MACRO TYPEDEF CONSTANT ENUM DECLARATION
//--------------------------------------------------------------------+
static scsi_inquiry_resp_t inquiry_resp;

volatile static int8_t requestEject = -1;
cd_s currentDisc = {0};

volatile bool inquiry_cb_flag;

uint8_t readBuffer[20480];


static volatile bool read_done;

static bool command_complete_cb(uint8_t dev_addr, msc_cbw_t const* cbw, msc_csw_t const* csw) {
  usb_state &= ~COMMAND_ON_GOING;
}

static bool check_eject() {
  if (requestEject!=-1) {
    // printf("Eject requested %d\n", requestEject);
    usb_state |= COMMAND_ON_GOING;
    //Execute right now
    printf("Eject %d\n", requestEject);
    if ( !tuh_msc_start_stop(currentDisc.dev_addr, currentDisc.lun, requestEject, true, command_complete_cb)) {
      printf("Got error while eject command\n");
    }
    else {
      if (requestEject == 0) {
        set3doCDReady(false);
        set3doDriveMounted(false);
        currentDisc.mounted = false;
        usb_state &= ~DISC_MOUNTED;
      }
      requestEject = -1;
    }
    return true;
  }
  return false;
}

static void check_mount() {
  if (!currentDisc.mounted) {
    //Send a sense loop
    uint8_t const lun = 0;
    usb_state |= COMMAND_ON_GOING;
    checkForMedia(currentDisc.dev_addr, lun);
  }
}

static bool read_complete_cb(uint8_t dev_addr, msc_cbw_t const* cbw, msc_csw_t const* csw) {
  read_done = true;
  usb_state &= ~COMMAND_ON_GOING;
  return true;
}

bool block_is_ready() {
  usb_state &= ~COMMAND_ON_GOING;
  return read_done;
}

bool driveEject(bool eject) {
  if (!usb_state & ENUMERATED) {
    requestEject = (eject?0:1);
    return true;
  }
  if (requestEject != -1) return false;
  requestEject = (eject?0:1);
  return true;
}

static uint32_t start_Block;
static uint32_t nb_block_Block;
static uint8_t *buffer_Block;
static bool blockRequired = false;

void check_block() {
  if (blockRequired) {
    usb_state |= COMMAND_ON_GOING;
    blockRequired = false;
    if (currentDisc.block_size_read == 2048) {
      if ( !tuh_msc_read10(currentDisc.dev_addr, currentDisc.lun, buffer_Block, start_Block, nb_block_Block, read_complete_cb)) {
        printf("Got error with block read\n");
        return;
      }
    } else {
      if ( !tuh_msc_read_cd(currentDisc.dev_addr, currentDisc.lun, buffer_Block, start_Block, nb_block_Block, read_complete_cb)) {
        printf("Got error with block read\n");
        return;
      }
    }
  }
}

bool readBlock(uint32_t start, uint16_t nb_block, uint8_t *buffer) {
  read_done = false;
  start_Block = start;
  nb_block_Block = nb_block;
  buffer_Block = buffer;
  blockRequired = true;
  return true;
}

bool readSubQChannel(uint8_t *buffer) {
  //FAire async
  read_done = false;
  usb_state |= COMMAND_ON_GOING;
  if (!tuh_msc_read_sub_channel(currentDisc.dev_addr, currentDisc.lun, buffer, read_complete_cb)) {
    printf("Got error with sub Channel read\n");
    return false;
  }
  return true;
}


static bool read_toc_complete_cb(uint8_t dev_addr, msc_cbw_t const* cbw, msc_csw_t const* csw) {
  usb_state &= ~COMMAND_ON_GOING;
  currentDisc.nb_track = ((readBuffer[0]<<8)+readBuffer[1] - 2)/8;

  currentDisc.first_track = readBuffer[2];
  currentDisc.last_track = readBuffer[3];

  printf("First %d, last %d nb %x\n", currentDisc.first_track, currentDisc.last_track, currentDisc.nb_track);
  for (int i = 0; i < currentDisc.nb_track-1; i++) {
    int index = 4+8*i;
    //OxAA as id mean lead out
    currentDisc.tracks[i].id = readBuffer[index + 2];
    currentDisc.tracks[i].CTRL_ADR = readBuffer[index + 1];
    currentDisc.tracks[i].msf[0] = readBuffer[index + 5]; //MSF
    currentDisc.tracks[i].msf[1] = readBuffer[index + 6];
    currentDisc.tracks[i].msf[2] = readBuffer[index + 7];
    printf("Track[%d] 0x%x (0x%x)=> %d:%d:%d\n", i, currentDisc.tracks[i].id, currentDisc.tracks[i].CTRL_ADR, currentDisc.tracks[i].msf[0], currentDisc.tracks[i].msf[1], currentDisc.tracks[i].msf[2]);
  }
  currentDisc.mounted = true;
  usb_state |= DISC_MOUNTED;
  set3doCDReady(true);
  set3doDriveMounted(true);
  return true;
}

static bool read_toc_light_complete_cb(uint8_t dev_addr, msc_cbw_t const* cbw, msc_csw_t const* csw) {
  currentDisc.nb_track = (((readBuffer[0]<<8)+readBuffer[1]) - 2)/8;

  currentDisc.first_track = readBuffer[2];
  currentDisc.last_track = readBuffer[3];

  printf("First %d, last %d nbTrack %d\n", currentDisc.first_track, currentDisc.last_track, currentDisc.nb_track);

  if (currentDisc.nb_track != 0) {
    if (!tuh_msc_read_toc(dev_addr, cbw->lun, readBuffer, 1, 0, currentDisc.nb_track, read_toc_complete_cb)) {
        printf("Got error with toc read\n");
        return false;
    }
  } else {
    return read_toc_complete_cb(dev_addr, cbw, csw);
  }
  return true;
}

bool inquiry_complete_cb(uint8_t dev_addr, msc_cbw_t const* cbw, msc_csw_t const* csw)
{
  inquiry_cb_flag = true;
  if (csw->status != 0)
  {
    printf("Inquiry failed\r\n");
    //Quel status pour la 3DO?
    return false;
  }

  // Print out Vendor ID, Product ID and Rev
  printf("%.8s %.16s rev %.4s Type 0x%x Lun %d\r\n", inquiry_resp.vendor_id, inquiry_resp.product_id, inquiry_resp.product_rev, inquiry_resp.peripheral_device_type, cbw->lun);

  // Get capacity of device
  currentDisc.nb_block = tuh_msc_get_block_count(dev_addr, cbw->lun);
  currentDisc.block_size = tuh_msc_get_block_size(dev_addr, cbw->lun);
  currentDisc.block_size_read = currentDisc.block_size;
  currentDisc.lun = cbw->lun;

  int lba = currentDisc.nb_block + 150;
  currentDisc.msf[0] = lba/(60*75);
  lba %= 60*75;
  currentDisc.msf[1] = lba / 75;
  currentDisc.msf[2] = lba % 75;

  //Assume type is CD-DA or CD-ROM always
  currentDisc.format = 0x0; /*00 CD-DA or CD-ROM / 10 CD-I / 20 XA */

  printf("Disk Size: %lu MB\r\n", currentDisc.nb_block / ((1024*1024)/currentDisc.block_size));
  printf("Block Count = %lu, Block Size: %lu\r\n", currentDisc.nb_block, currentDisc.block_size);
  printf("Disc duration is %2d:%2d:%2d\n", currentDisc.msf[0], currentDisc.msf[1], currentDisc.msf[2]);

  if (!tuh_msc_read_toc(dev_addr, cbw->lun, readBuffer, 1, 0, 0, read_toc_light_complete_cb)) {
      printf("Got error with toc read\n");
      return false;
  }
  return true;
}

//------------- IMPLEMENTATION -------------//

void tuh_msc_ready_cb(uint8_t dev_addr, bool ready)
{
  set3doDriveReady();
  usb_state |= DISC_IN;
}

void tuh_msc_enumerated_cb(uint8_t dev_addr)
{
  currentDisc.dev_addr = dev_addr;
  usb_state |= ENUMERATED;
  usb_state &= ~COMMAND_ON_GOING;
}

void tuh_msc_mount_cb(uint8_t dev_addr)
{
  uint8_t const lun = 0;
  usb_state |= COMMAND_ON_GOING;
  printf("A USB MassStorage device is mounted\r\n");
  inquiry_cb_flag = false;
  tuh_msc_inquiry(dev_addr, lun, &inquiry_resp, inquiry_complete_cb);
}

void tuh_msc_umount_cb(uint8_t dev_addr)
{
  (void) dev_addr;
  printf("A USB MassStorage device is unmounted\r\n");
  set3doCDReady(false);
  set3doDriveMounted(false);
  currentDisc.mounted = false;
  usb_state &= ~DISC_MOUNTED;
}

#endif
