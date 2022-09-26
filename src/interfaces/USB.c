#include "USB.h"
#include "CDFormat.h"
#include "3DO.h"
#include "CDROM.h"
#include "MSC.h"
#include "pico/stdio.h"

uint8_t usb_state = 0;

static scsi_inquiry_resp_t inquiry_resp;
static bool inquiry_complete_cb(uint8_t dev_addr, msc_cbw_t const* cbw, msc_csw_t const* csw);
static void check_mount();

void USB_Host_init() {
    currentDisc.dev_addr = 0xFF;
    currentDisc.canBeLoaded = false;
    currentDisc.canBeEjected = false;
    SET_USB_STEP(DETACHED);
    SET_USB_PERIPH_TYPE(UNKNOWN_TYPE);
#ifdef USE_TRACE
    stdio_init_all();
#endif
    tusb_init();
}

static bool check_eject();

static bool Default_Host_loop() {
#if CFG_TUH_MSC
  if (!IS_USB_CMD_ONGOING()) {
    switch(GET_USB_STEP()) {
      case EJECTING:
        check_eject();
      case MOUNTED:
        return true;
        break;
      default:
        return false;
    }
  }
#endif
  return false;
}

void USB_Host_loop()
{
  // tinyusb host task
  bool ret = true;
  tuh_task();
  switch (GET_USB_PERIPH_TYPE()) {
    case CD_TYPE:
      ret = CDROM_Host_loop();
      break;
    case MSC_TYPE:
      ret = MSC_Host_loop();
      break;
    default:
      ret = Default_Host_loop();
  }
  if (!ret) {
    if(GET_USB_STEP()==ATTACHED) tuh_msc_inquiry(currentDisc.dev_addr, 0, &inquiry_resp, inquiry_complete_cb);
    if(GET_USB_STEP()==ENUMERATED) check_mount();
    if(GET_USB_STEP()==CONFIGURED) check_mount();
  }
}


#if CFG_TUH_MSC

//--------------------------------------------------------------------+
// MACRO TYPEDEF CONSTANT ENUM DECLARATION
//--------------------------------------------------------------------+

volatile int8_t requestLoad = -1;
cd_s currentDisc = {0};

static bool startClose = true;

volatile bool read_done;
volatile bool is_audio;
volatile bool has_subQ;

volatile bool usb_result;

uint32_t start_Block;
uint32_t nb_block_Block;
uint8_t *buffer_Block;
bool blockRequired = false;
bool subqRequired = false;
uint8_t *buffer_subq;

fileCmd_s fileCmdRequired;

static bool check_eject() {
  //Execute right now
  LOG_SATA("Eject\n");
  if (CDROM_ExecuteEject()) SET_USB_STEP(ATTACHED);
    return true;
}

void USB_reset(void) {
  LOG_SATA("reset usb\n");
  startClose = false;
  tuh_msc_umount_cb(currentDisc.dev_addr);
  tusb_reset();
}

static void check_mount() {
  //Send a sense loop
  uint8_t const lun = 0;
  SET_USB_CMD_ONGOING();
  checkForMedia(currentDisc.dev_addr, lun);
  CLEAR_USB_CMD_ONGOING();
}

static bool inquiry_complete_cb(uint8_t dev_addr, msc_cbw_t const* cbw, msc_csw_t const* csw)
{
  bool ret = false;
  if (csw->status != MSC_CSW_STATUS_GOOD)
  {
    TU_LOG1("Inquiry failed %x\r\n", csw->status);
    SET_USB_STEP(ATTACHED);
    return ret;
  }
  //At least consider we are enumerated so that configuration can be read
  if (GET_USB_STEP() < ENUMERATED) SET_USB_STEP(ENUMERATED);
  // Print out Vendor ID, Product ID and Rev
  TU_LOG1("%.8s %.16s rev %.4s Type 0x%x Lun %d\r\n", inquiry_resp.vendor_id, inquiry_resp.product_id, inquiry_resp.product_rev, inquiry_resp.peripheral_device_type, cbw->lun);

  currentDisc.dev_addr = dev_addr;

  currentDisc.lun = cbw->lun;

  if (inquiry_resp.peripheral_device_type == 0x5) {
    SET_USB_PERIPH_TYPE(CD_TYPE);
    ret = CDROM_Inquiry(dev_addr, cbw, csw);
  }
  if (inquiry_resp.peripheral_device_type == 0x0) {
    SET_USB_PERIPH_TYPE(MSC_TYPE);
    ret = MSC_Inquiry(dev_addr, cbw, csw);
  }
  if (ret) {
    //disc is detected
    // Be sure we have the configuration done
    CLEAR_USB_CMD_ONGOING();
    if (GET_USB_STEP() < CONFIGURED) {
      if (GET_USB_PERIPH_TYPE() == CD_TYPE)
      CDROM_ready(dev_addr, true);
    }
    // If we still do not have the configuration, consider we are configured
    if (GET_USB_STEP() < CONFIGURED) {
      //capabilities does not work, consider eject is possible
      currentDisc.canBeLoaded = true;
      currentDisc.canBeEjected = true;
      SET_USB_STEP(CONFIGURED);
    }
  }
  return ret;
}

void tuh_msc_ready_cb(uint8_t dev_addr, bool ready) {
  if (GET_USB_PERIPH_TYPE() == CD_TYPE)
    CDROM_ready(dev_addr, ready);
}

void tuh_msc_enumerate_cb (uint8_t dev_addr) {
  uint8_t buffer_void[18];
  TU_LOG1("Usb device Mounted %x\n", dev_addr);
  currentDisc.dev_addr = dev_addr;
}

void tuh_mount_cb(uint8_t dev_addr) {
  if (currentDisc.dev_addr != 0xFF) SET_USB_STEP(ATTACHED);
}

bool read_complete_cb(uint8_t dev_addr, msc_cbw_t const* cbw, msc_csw_t const* csw) {
  if (csw->status != MSC_CSW_STATUS_GOOD) {

    set3doDriveError();
  }
  read_done = true;
  CLEAR_USB_CMD_ONGOING();
  return true;
}

bool block_is_ready() {
  return read_done;
}

bool cmd_is_ready(bool *success) {
  if (success != NULL) *success = usb_result;
  return fileCmdRequired == DONE;
}

bool write_is_ready(bool *success) {
  *success = usb_result;
  return fileCmdRequired == DONE;
}

bool USBDriveEject(bool eject, bool *interrupt) {
  if (GET_USB_PERIPH_TYPE() == CD_TYPE) {
    // *interrupt = !eject;
    *interrupt = true;
    SET_USB_STEP(EJECTING);
    LOG_SATA("usb not enumerated\n");
    return true;
  }
  if (GET_USB_PERIPH_TYPE() == MSC_TYPE) {
    *interrupt = true;
    return true;
  }
  return false;
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

bool readBlock(uint32_t start, uint16_t nb_block, uint16_t block_size, uint8_t *buffer) {
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
//------------- IMPLEMENTATION -------------//

void tuh_msc_enumerated_cb(uint8_t dev_addr)
{
  TU_LOG1("##### enumerated ######\n");
}

void tuh_msc_mount_cb(uint8_t dev_addr)
{
  LOG_SATA("A USB MassStorage device is mounted\r\n");
  SET_USB_STEP(MOUNTED);
  tuh_msc_inquiry(currentDisc.dev_addr, 0, &inquiry_resp, inquiry_complete_cb);
}

void tuh_msc_umount_cb(uint8_t dev_addr)
{
  (void) dev_addr;
  LOG_SATA("A USB MassStorage device is unmounted\r\n");
  set3doCDReady(false);
  set3doDriveMounted(false);
  currentDisc.dev_addr = 0xFF;
  currentDisc.canBeLoaded = false;
  currentDisc.canBeEjected = false;
  SET_USB_STEP(DETACHED);
  SET_USB_PERIPH_TYPE(UNKNOWN_TYPE);
  mediaInterrupt();
}

#endif
