#ifndef __MSC_H_INCLUDE__
#define __MSC_H_INCLUDE__

#include "USB.h"
#include "TOC.h"

extern bool MSC_Host_loop();

extern bool MSC_Inquiry(uint8_t dev_addr, msc_cbw_t const* cbw, msc_csw_t const* csw);

extern bool MSC_ExecuteEject(bool eject);

extern void setTocLevel(int index);
extern int getTocLevel(void);

extern bool getNextTOCEntry(toc_entry* toc);
extern bool getReturnTocEntry(toc_entry* toc);
extern void getToc(int index, int offset, uint8_t* buffer);
extern void getTocFull(int index, int nb);


extern bool seekTocTo(int index);

#endif