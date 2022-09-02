#ifndef _3DO_INTERFACE_H_
#define _3DO_INTERFACE_H_

#include <stdbool.h>

#define UART_LED 28

#define DATA_0 2
#define DATA_1 3
#define DATA_2 4
#define DATA_3 5
#define DATA_4 6
#define DATA_5 7
#define DATA_6 8
#define DATA_7 9
#define CTRL0 11
#define CTRL1 12
#define CTRL2 13
#define CTRL3 14
#define CTRL4 15
#define CTRL5 20
#define CTRL6 21
#define CTRL7 10
#define CTRL8 22

#define EJECT_3V 26
#define DIR_DATA 27

#define XTRA0 16
#define XTRA1 18
#define XTRA2 17
#define XTRA3 19

//Input or output
#define CDD0    DATA_0
#define CDD1    DATA_1
#define CDD2    DATA_2
#define CDD3    DATA_3
#define CDD4    DATA_4
#define CDD5    DATA_5
#define CDD6    DATA_6
#define CDD7    DATA_7

//Always input
#define EJECT   EJECT_3V
#define CDEN    CTRL8
#define CDRST   CTRL5
#define CDHRD   XTRA2
#define CDHWR   CTRL4
#define CDCMD   CTRL7

//output
#define LED     UART_LED
#define CDSTEN  CTRL0
#define CDDTEN  CTRL1
#define CDWAIT  CTRL2
#define CDMDCHG CTRL6


#define CDEN_SNIFF XTRA0
#define CDRST_SNIFF XTRA1


#define DATA_MASK ((1<<CDD0)|(1<<CDD1)|(1<<CDD2)|(1<<CDD3)|(1<<CDD4)|(1<<CDD5)|(1<<CDD6)|(1<<CDD7))
#define CTRL_MASK ((1<<EJECT)|(1<<CDEN)|(1<<CDRST)|(1<<CDHRD)|(1<<CDHWR)|(1<<CDCMD))

#define LOG_SATA printf


extern void _3DO_init();
extern void set3doCDReady(bool on);
extern void set3doDriveMounted(bool on);
extern void set3doDriveReady();
extern void set3doDriveError();

extern void mediaInterrupt(void);

#endif