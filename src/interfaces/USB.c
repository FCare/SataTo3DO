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

static bool read10_complete_cb(uint8_t dev_addr, msc_cbw_t const* cbw, msc_csw_t const* csw) {
  printf("Read ok (%d):", cbw->total_bytes);
  for (int i = 0; i < cbw->total_bytes; i++) {
    printf("0x%x ", readBuffer[i]);
  }
  printf("\n");
}

static bool read_toc_complete_cb(uint8_t dev_addr, msc_cbw_t const* cbw, msc_csw_t const* csw) {
  currentDisc.nb_track = (((readBuffer[0]<<8)+readBuffer[1]) - 2)/8;

  currentDisc.first_track = readBuffer[2];
  currentDisc.last_track = readBuffer[3];

  printf("First %d, last %d\n", currentDisc.first_track, currentDisc.last_track);

  for (int i = 0; i < currentDisc.nb_track; i++) {
    int index = 4+8*i;
    //OxAA as id mean lead out
    currentDisc.tracks[i].id = readBuffer[index + 2];
    currentDisc.tracks[i].ADR = readBuffer[index + 1]>>4;
    currentDisc.tracks[i].CTRL = readBuffer[index + 1]&0xF;
    currentDisc.tracks[i].msf[2] = readBuffer[index + 5];
    currentDisc.tracks[i].msf[1] = readBuffer[index + 6];
    currentDisc.tracks[i].msf[0] = readBuffer[index + 7];
    printf("Track[%d] 0x%x => %d:%d:%d\n", i, currentDisc.tracks[i].id, currentDisc.tracks[i].msf[2], currentDisc.tracks[i].msf[1], currentDisc.tracks[i].msf[0]);
  }

  //Get lead out info as disk info
  currentDisc.msf[0] = currentDisc.tracks[currentDisc.last_track].msf[0];
  currentDisc.msf[1] = currentDisc.tracks[currentDisc.last_track].msf[1];
  currentDisc.msf[2] = currentDisc.tracks[currentDisc.last_track].msf[2];

  printf("Disc duration is %2d:%2d:%2d\n", currentDisc.msf[2], currentDisc.msf[1], currentDisc.msf[0]);
  currentDisc.format = 0x0; /*00 CD-DA or CD-ROM / 10 CD-I / 20 XA */
  //Assume type is CD-DA or CD-ROM always
  currentDisc.mounted = true;
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

  set3doCDReady(true);
  set3doDriveMounted(true);
  // Print out Vendor ID, Product ID and Rev
  printf("%.8s %.16s rev %.4s Type 0x%x\r\n", inquiry_resp.vendor_id, inquiry_resp.product_id, inquiry_resp.product_rev, inquiry_resp.peripheral_device_type);

  // Get capacity of device
  currentDisc.nb_block = tuh_msc_get_block_count(dev_addr, cbw->lun);
  currentDisc.block_size = tuh_msc_get_block_size(dev_addr, cbw->lun);

  printf("Disk Size: %lu MB\r\n", currentDisc.nb_block / ((1024*1024)/currentDisc.block_size));
  printf("Block Count = %lu, Block Size: %lu\r\n", currentDisc.nb_block, currentDisc.block_size);

  if (!tuh_msc_read_toc(dev_addr, cbw->lun, readBuffer, 1, 0, 0, read_toc_complete_cb)) {
      printf("Got error with toc read\n");
      return false;
  }
    // if ( !tuh_msc_read10(dev_addr, cbw->lun, readBuffer, 10, 10, read10_complete_cb)) {
  	//   printf("Got error with block read\n");
    //   return false;
  	// }
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
