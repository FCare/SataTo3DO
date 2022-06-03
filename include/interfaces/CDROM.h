#ifndef __CDROM_HOST_H_INCLUDE__
#define __CDROM_HOST_H_INCLUDE__

extern void CDROM_Host_loop();

extern bool CDROM_Inquiry(uint8_t dev_addr, msc_cbw_t const* cbw, msc_csw_t const* csw);

extern bool CDROM_ExecuteEject(bool eject);

#endif