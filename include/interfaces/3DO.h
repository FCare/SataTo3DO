#ifndef _3DO_INTERFACE_H_
#define _3DO_INTERFACE_H_

#define UART_LED 1

#define DATA_0 2
#define DATA_1 3
#define DATA_2 4
#define DATA_3 5
#define DATA_4 6
#define DATA_5 7
#define DATA_6 8
#define DATA_7 9

#define CTRL7 10
#define CTRL0 11
#define CTRL1 12
#define CTRL2 13
#define CTRL3 14
#define CTRL4 15
#define CTRL5 26
#define CTRL6 27
#define CTRL8 28

#define EJECT_3V 29

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
#define CDHRD   CTRL3
#define CDHWR   CTRL4
#define CDCMD   CTRL7

//output
#define LED     UART_LED
#define CDSTEN  CTRL0
#define CDDTEN  CTRL1
#define CDWAIT  CTRL2
#define CDMDCHG CTRL6


#define DATA_MASK ((1<<CDD0)|(1<<CDD1)|(1<<CDD2)|(1<<CDD3)|(1<<CDD4)|(1<<CDD5)|(1<<CDD6)|(1<<CDD7))
#define CTRL_MASK ((1<<EJECT)|(1<<CDEN)|(1<<CDRST)|(1<<CDHRD)|(1<<CDHWR)|(1<<CDCMD))

#ifndef USE_UART_RX
#define LOG_SATA
#else
#define LOG_SATA printf
#endif



typedef struct {
  uint8_t id;
  uint8_t CTRL_ADR;
  uint8_t msf[3];
  uint32_t lba;
} track_s;

typedef struct {
  bool mounted;
  bool multiSession;
  uint8_t first_track;
  uint8_t last_track;
  uint8_t format;
  uint8_t nb_track;
  uint8_t msf[3];
  uint32_t nb_block;
  uint16_t block_size;
  uint16_t block_size_read;
  uint8_t dev_addr;
  uint8_t lun;
  bool hasOnlyAudio;
  track_s tracks[100];
} cd_s;

extern void _3DO_init();
extern void set3doCDReady(bool on);
extern void set3doDriveMounted(bool on);
extern void set3doDriveReady();
extern void set3doDriveError();

#endif