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

static bool read10_complete_cb(uint8_t dev_addr, msc_cbw_t const* cbw, msc_csw_t const* csw) {
  printf("Read ok (%d):", cbw->total_bytes);
  for (int i = 0; i < cbw->total_bytes; i++) {
    printf("0x%x ", readBuffer[i]);
  }
  printf("\n");
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

  set3doDriveMounted(true);
  // Print out Vendor ID, Product ID and Rev
  printf("%.8s %.16s rev %.4s Type 0x%x\r\n", inquiry_resp.vendor_id, inquiry_resp.product_id, inquiry_resp.product_rev, inquiry_resp.peripheral_device_type);

  // Get capacity of device
  uint32_t const block_count = tuh_msc_get_block_count(dev_addr, cbw->lun);
  uint32_t const block_size = tuh_msc_get_block_size(dev_addr, cbw->lun);

  printf("Disk Size: %lu MB\r\n", block_count / ((1024*1024)/block_size));
  printf("Block Count = %lu, Block Size: %lu\r\n", block_count, block_size);
    if ( !tuh_msc_read10(dev_addr, cbw->lun, readBuffer, 10, 10, read10_complete_cb)) {
  	  printf("Got error with block read\n");
      return false;
  	}
  return true;
}

//------------- IMPLEMENTATION -------------//
void tuh_msc_mount_cb(uint8_t dev_addr)
{
  uint8_t const lun = 0;
  printf("A USB MassStorage device is mounted\r\n");
  inquiry_cb_flag = false;
  set3doDriveReady(true);
  tuh_msc_inquiry(dev_addr, lun, &inquiry_resp, inquiry_complete_cb);
}

void tuh_msc_umount_cb(uint8_t dev_addr)
{
  (void) dev_addr;
  printf("A USB MassStorage device is unmounted\r\n");
  set3doDriveReady(false);
  set3doDriveMounted(false);
}

#endif
