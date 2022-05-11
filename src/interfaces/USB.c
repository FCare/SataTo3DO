#include "USB.h"
#include "3DO.h"
#include "pico/stdio.h"

void USB_Host_init() {
    stdio_init_all();
    tusb_init();
}

void USB_Host_loop()
{
  // tinyusb host task
  tuh_task();

}


#if CFG_TUH_MSC

//--------------------------------------------------------------------+
// MACRO TYPEDEF CONSTANT ENUM DECLARATION
//--------------------------------------------------------------------+
static scsi_inquiry_resp_t inquiry_resp;

volatile bool inquiry_cb_flag;

uint8_t readBuffer[20480];

cd_s currentDisc = {0};

static volatile bool read_done;

static bool read10_complete_cb(uint8_t dev_addr, msc_cbw_t const* cbw, msc_csw_t const* csw) {
  read_done = true;
  printf("Read ok\n");
  return true;
}

bool readBlock(uint32_t start, uint16_t nb_block, uint8_t *buffer) {
  read_done = false;
  if ( !tuh_msc_read10(currentDisc.dev_addr, currentDisc.lun, buffer, start, nb_block, read10_complete_cb)) {
    printf("Got error with block read\n");
    return false;
  }
  printf("Read req\n");
  while (read_done == false);
  return true;
}
static bool read_toc_complete_cb(uint8_t dev_addr, msc_cbw_t const* cbw, msc_csw_t const* csw) {
  for (int i = 0; i < currentDisc.nb_track-1; i++) {
    int index = 4+8*i;
    //OxAA as id mean lead out
    currentDisc.tracks[i].id = readBuffer[index + 2];
    currentDisc.tracks[i].CTRL_ADR = readBuffer[index + 1];
    currentDisc.tracks[i].msf[0] = readBuffer[index + 5]; //MSF
    currentDisc.tracks[i].msf[1] = readBuffer[index + 6];
    currentDisc.tracks[i].msf[2] = readBuffer[index + 7];
    printf("Track[%d] 0x%x (0x%x)=> %d:%d:%d\n", i, currentDisc.tracks[i].id, currentDisc.tracks[i].CTRL_ADR, currentDisc.tracks[i].msf[0], currentDisc.tracks[i].msf[1], currentDisc.tracks[i].msf[0]);
  }
  currentDisc.mounted = true;
  set3doCDReady(true);
  set3doDriveMounted(true);
  return true;
}

static bool read_toc_light_complete_cb(uint8_t dev_addr, msc_cbw_t const* cbw, msc_csw_t const* csw) {
  currentDisc.nb_track = (((readBuffer[0]<<8)+readBuffer[1]) - 2)/8;

  currentDisc.first_track = readBuffer[2];
  currentDisc.last_track = readBuffer[3];

  printf("First %d, last %d\n", currentDisc.first_track, currentDisc.last_track);

  if (currentDisc.nb_track > 2) {
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
  currentDisc.dev_addr = dev_addr;
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

  if (!tuh_msc_read_toc(dev_addr, cbw->lun, readBuffer, 1, 0, 2, read_toc_light_complete_cb)) {
      printf("Got error with toc read\n");
      return false;
  }
  return true;
}

//------------- IMPLEMENTATION -------------//

void tuh_msc_ready_cb(uint8_t dev_addr, bool ready)
{
  set3doDriveReady();
}

void tuh_msc_mount_cb(uint8_t dev_addr)
{
  uint8_t const lun = 0;
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
}

#endif
