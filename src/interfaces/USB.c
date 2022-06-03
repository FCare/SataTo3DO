#include "USB.h"
#include "CDFormat.h"
#include "3DO.h"
#include "CDROM.h"
#include "pico/stdio.h"

uint8_t usb_state = 0;

void USB_Host_init() {
    stdio_init_all();
    tusb_init();
}

void USB_Host_loop()
{
  // tinyusb host task
  tuh_task();
  CDROM_Host_loop();
}


#if CFG_TUH_MSC

//--------------------------------------------------------------------+
// MACRO TYPEDEF CONSTANT ENUM DECLARATION
//--------------------------------------------------------------------+
static scsi_inquiry_resp_t inquiry_resp;

volatile int8_t requestEject = -1;
cd_s currentDisc = {0};

volatile bool inquiry_cb_flag;

static bool startClose = true;

uint8_t readBuffer[20480];


static volatile bool read_done;
volatile bool is_audio;
volatile bool has_subQ;

uint32_t start_Block;
uint32_t nb_block_Block;
uint8_t *buffer_Block;
bool blockRequired = false;
bool subqRequired = false;
uint8_t *buffer_subq;

void USB_reset(void) {
  printf("reset usb\n");
  usb_state = 0;
  startClose = false;
  tuh_msc_umount_cb(currentDisc.dev_addr);
  tusb_reset();
}

bool read_complete_cb(uint8_t dev_addr, msc_cbw_t const* cbw, msc_csw_t const* csw) {
  if (csw->status != MSC_CSW_STATUS_PASSED) {

    set3doDriveError();
  }
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


bool readSubQChannel(uint8_t *buffer) {
  buffer_subq = buffer;
  read_done = false;
  subqRequired = true;
  return true;
}

bool isAudioBlock(uint32_t start) {
  bool res = false;
  for (int i = 0; i < currentDisc.nb_track-1; i++) {
    if (currentDisc.tracks[i].lba <= start) {
      res = ((currentDisc.tracks[i].CTRL_ADR & 0x4) == 0x0);
    }
  }
  return res;
}

bool readBlock(uint32_t start, uint16_t nb_block, uint8_t block_size, uint8_t *buffer) {
  is_audio = false;
  has_subQ = false;
  read_done = false;
  start_Block = start;
  nb_block_Block = nb_block;
  buffer_Block = buffer;
  for (int i = 0; i < currentDisc.nb_track-1; i++) {
    if (currentDisc.tracks[i].lba <= start) {
      is_audio = ((currentDisc.tracks[i].CTRL_ADR & 0x4) == 0x0);
      has_subQ = (block_size >= 2448);
    }
  }
  blockRequired = true;
  return true;
}

bool inquiry_complete_cb(uint8_t dev_addr, msc_cbw_t const* cbw, msc_csw_t const* csw)
{
  inquiry_cb_flag = true;
  if (csw->status != 0)
  {
    LOG_SATA("Inquiry failed\r\n");
    return false;
  }

  // Print out Vendor ID, Product ID and Rev
  LOG_SATA("%.8s %.16s rev %.4s Type 0x%x Lun %d\r\n", inquiry_resp.vendor_id, inquiry_resp.product_id, inquiry_resp.product_rev, inquiry_resp.peripheral_device_type, cbw->lun);

  if (inquiry_resp.peripheral_device_type == 0x5) {
    return CDROM_Inquiry(dev_addr, cbw, csw);
  }

  // if (inquiry_resp.peripheral_device_type == 0x0) {
  //   return MSC_Inquiry(dev_addr, cbw, csw);
  // }

  return false;
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
  if (!startClose) CDROM_ExecuteEject(startClose);
  startClose = true;
}

void tuh_msc_mount_cb(uint8_t dev_addr)
{
  uint8_t const lun = 0;
  usb_state |= COMMAND_ON_GOING;
  LOG_SATA("A USB MassStorage device is mounted\r\n");
  inquiry_cb_flag = false;
  tuh_msc_inquiry(dev_addr, lun, &inquiry_resp, inquiry_complete_cb);
}

void tuh_msc_umount_cb(uint8_t dev_addr)
{
  (void) dev_addr;
  LOG_SATA("A USB MassStorage device is unmounted\r\n");
  set3doCDReady(false);
  set3doDriveMounted(false);
  currentDisc.mounted = false;
  usb_state &= ~DISC_MOUNTED;
}

#endif
