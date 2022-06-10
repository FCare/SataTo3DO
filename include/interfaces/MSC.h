#ifndef __MSC_H_INCLUDE__
#define __MSC_H_INCLUDE__

extern bool MSC_Host_loop();

extern bool MSC_Inquiry(uint8_t dev_addr, msc_cbw_t const* cbw, msc_csw_t const* csw);

extern bool MSC_ExecuteEject(bool eject);

#endif