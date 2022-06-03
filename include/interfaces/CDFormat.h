#ifndef __CDROM_FORMAT_H_INCLUDE__
#define __CDROM_FORMAT_H_INCLUDE__

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

extern cd_s currentDisc;

#endif