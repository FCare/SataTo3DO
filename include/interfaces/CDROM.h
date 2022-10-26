#ifndef __CDROM_HOST_H_INCLUDE__
#define __CDROM_HOST_H_INCLUDE__

extern bool CDROM_Host_loop(device_s *dev);

extern bool CDROM_Inquiry(uint8_t dev_addr, uint8_t lun);

extern bool CDROM_ExecuteEject();

extern void CDROM_ready(uint8_t dev_addr, bool ready);

extern void wakeUpCDRom(uint8_t dev_addr, uint8_t lun);

#endif