#include "3DO.h"
#include "MSC.h"
#include "CDFormat.h"
#include "pico/stdio.h"

#include "diskio.h"


/* -------------------------------------------
- Thanks to https://github.com/trapexit/3dt for 3do image detection and filesystem extraction
---------------------------------------------*/

FIL curDataFile;
char *curFilePath = NULL;
static bool curFileMode = false;

typedef enum {
  BOOT_ISO = 1,
  BOOT_PLAYLIST,
} mount_mode;

static mount_mode onMountMode = BOOT_ISO;

#define PLAYLIST_MAX 16
typedef struct playlist_entry_s{
  char *path[PLAYLIST_MAX];
  int nb_entries;
  int current_entry;
} playlist_entry;

playlist_entry playlist = {0};

extern uint32_t start_Block;
extern uint32_t nb_block_Block;
extern uint8_t *buffer_Block;
extern bool blockRequired;

extern bool read_done;
extern fileCmd_s fileCmdRequired;

extern bool subqRequired;
extern uint8_t *buffer_subq;

extern volatile bool usb_result;


#define SELECTED_IMAGE 22

#define TOC_NAME_LIMIT 128

#if CFG_TUH_MSC
static bool check_eject(uint8_t dev_addr);
static void check_load(uint8_t dev_addr);
static void check_block(uint8_t dev_addr);
static void check_file(uint8_t dev_addr);
static void check_subq(uint8_t dev_addr);
#endif

static bool handleBootImage(uint8_t dev_addr);

static FSIZE_t last_pos = 0;

static FIL file;

bool MSC_Host_loop(device_s *dev)
{
#if CFG_TUH_MSC
  if (!usb_cmd_on_going) {
    switch(dev->state) {
      case EJECTING:
        check_eject(dev->dev_addr);
      case MOUNTED:
        check_load(dev->dev_addr);
        check_file(dev->dev_addr);
        check_block(dev->dev_addr);
        check_subq(dev->dev_addr);
        return true;
        break;
      default:
        return false;
    }
  }
#endif
  return false;
}


#if CFG_TUH_MSC

//--------------------------------------------------------------------+
// MACRO TYPEDEF CONSTANT ENUM DECLARATION
//--------------------------------------------------------------------+

static bool startClose = true;

static bool validateFile(FILINFO* fileInfo);
static bool getNextValidToc(FILINFO *fileInfo);


extern volatile bool is_audio;
extern volatile bool has_subQ;

#define NB_SUPPORTED_GAMES 100

char* curBinPath; //same number as tracks number
FIL curBinFile;

char *curBuf = NULL;
uint16_t curBufLength = 0;

static void print_error_text(FRESULT e) {
  switch (e) {
    case 0:   return; //FR_OK = 0,				/* (0) Succeeded */
    case 1:   printf("FR_DISK_ERR,			/* (1) A hard error occurred in the low level disk I/O layer */"); break;
    case 2:   printf("FR_INT_ERR,			/* (2) Assertion failed */"); break;
    case 3:   printf("FR_NOT_READY,		/* (3) The physical drive cannot work */"); break;
    case 4:   printf("FR_NO_FILE,				/* (4) Could not find the file */"); break;
    case 5:   printf("FR_NO_PATH,				/* (5) Could not find the path */"); break;
    case 6:   printf("FR_INVALID_NAME,		/* (6) The path name format is invalid */"); break;
    case 7:   printf("FR_DENIED,				/* (7) Access denied due to prohibited access or directory full */"); break;
    case 8:   printf("FR_EXIST,				/* (8) Access denied due to prohibited access */"); break;
    case 9:   printf("FR_INVALID_OBJECT,		/* (9) The file/directory object is invalid */"); break;
    case 10:  printf("FR_WRITE_PROTECTED,		/* (10) The physical drive is write protected */"); break;
    case 11:  printf("FR_INVALID_DRIVE,		/* (11) The logical drive number is invalid */"); break;
    case 12:  printf("FR_NOT_ENABLED,			/* (12) The volume has no work area */"); break;
    case 13:  printf("FR_NO_FILESYSTEM,		/* (13) There is no valid FAT volume */"); break;
    case 14:  printf("FR_MKFS_ABORTED,		/* (14) The f_mkfs() aborted due to any problem */"); break;
    case 15:  printf("FR_TIMEOUT,				/* (15) Could not get a grant to access the volume within defined period */"); break;
    case 16:  printf("FR_LOCKED,				/* (16) The operation is rejected according to the file sharing policy */"); break;
    case 17:  printf("FR_NOT_ENOUGH_CORE,		/* (17) LFN working buffer could not be allocated */"); break;
    case 18:  printf("FR_TOO_MANY_OPEN_FILES,	/* (18) Number of open files > FF_FS_LOCK */"); break;
    case 19:  printf("FR_INVALID_PARAMETER	/* (19) Given parameter is invalid */"); break;
    default:  printf("Unrecognised error %d", e);
  }
  printf("\n");
}

static bool check_eject(uint8_t dev_addr) {
  //Execute right now
  LOG_SATA("Eject\n");
  return true;
}


void requestBootImage() {
  requestLoad = true;
}

void waitForLoad() {
  while(requestLoad);
}

static void check_load(uint8_t dev_addr) {
  device_s *dev = getDevice(dev_addr);
  if (requestLoad && dev->isFatFs) {
    //Execute right now
    requestLoad = !handleBootImage(dev_addr);
  }
}


static void check_subq(uint8_t dev_addr) {
  if (subqRequired) {
    bool found = false;
    usb_cmd_on_going = true;
    cd_s *target_track = &currentImage;
    memset(buffer_subq, 0x0, 16);
    uint lba = start_Block + 150;
    buffer_subq[9] =  lba/(60*75);
    lba %= 60*75;
    buffer_subq[10] = lba / 75;
    buffer_subq[11] = lba % 75;
    lba = start_Block;
    for (int i = 0; i<target_track->last_track; i++)
    {
      if (target_track->tracks[i].lba <= start_Block) {
        buffer_subq[5] = target_track->tracks[i].CTRL_ADR;
        buffer_subq[6] = target_track->tracks[i].id;
        buffer_subq[7] = 1;
        lba = start_Block - target_track->tracks[i].lba;
        buffer_subq[13] = lba/(60*75);
        lba %= 60*75;
        buffer_subq[14] = lba / 75;
        buffer_subq[15] = lba % 75;
        found = true;
      } else {
        break;
      }
    }
    usb_cmd_on_going = false;
    subqRequired = false;
    read_done = true;
  }
}

void printPlaylist(void) {
  LOG_SATA("Playlist:\n");
  for (int i =0; i<playlist.nb_entries; i++) {
    LOG_SATA("%d: %s\n", i, playlist.path[i]);
  }
}

void clearPlaylist(void) {
  LOG_SATA("Clear laylist!\n");
  for (int i=0; i<playlist.nb_entries; i++) {
    if (playlist.path[i] != NULL) {
      free(playlist.path[i]);
      playlist.path[i] = NULL;
    }
  }
  playlist.nb_entries = 0;
  playlist.current_entry = 0;
}


void addToPlaylist(int entry, bool *valid, bool *added) {
  char *entry_path = NULL;
  entry_path = getPathForTOC(entry);
  if (entry_path == NULL) {
    *valid = false;
    *added = false;
    return;
  }
  *valid = true;
  if (playlist.nb_entries >= PLAYLIST_MAX) {
    *valid = false;
    *added = false;
    return;
  }
  *added = true;
  playlist.path[playlist.nb_entries++] = entry_path;
  LOG_SATA("Add to playlist %s %d\n", entry_path, playlist.nb_entries);
}

static void check_block(uint8_t dev_addr) {
  if (blockRequired) {
    usb_cmd_on_going = true;

    cd_s *target_track = &currentImage;
    FIL *fileOpen = &curBinFile;
    uint read_nb = 0;
    FSIZE_t offset = (start_Block - target_track->tracks[0].lba)*currentImage.block_size + currentImage.offset;
    if (currentImage.block_size != currentImage.block_size_read) {
      if (currentImage.format != 0) {
        //Assuming a XA format has only MODE_2 and CDDA track
        if (currentImage.block_size_read == 2048) {
          //Mode 2 Form1
          offset += 12 + 4 + 8; //Skip Sync, Header and subHeader
        }
      } else {
        //Mode 1 or CDDA
        if (currentImage.block_size_read == 2048) {
          //Mode 1
          offset += 12 + 4; //Skip Sync and Header
        }
      }
    }
    if (last_pos != offset)
      if (f_lseek(fileOpen, offset) != FR_OK) LOG_SATA("Can not seek %s\n", curBinPath);
    last_pos = offset;
    if (is_audio) {
      if (has_subQ) {
        //Work only for 1 block here
        FSIZE_t data_offset = 0;

          if (f_read(fileOpen, &buffer_Block[data_offset], currentImage.block_size, &read_nb) != FR_OK) LOG_SATA("Can not read %s\n", curBinPath);
          data_offset += read_nb;
          for (int i = currentImage.block_size; i < currentImage.block_size_read; i++) {
            buffer_Block[data_offset++] = 0;
          }
      } else {
        if (f_read(fileOpen, &buffer_Block[0], currentImage.block_size_read, &read_nb) != FR_OK) LOG_SATA("Can not read %s\n", curBinPath);
      }
    } else {
      if (f_read(fileOpen, buffer_Block, nb_block_Block*currentImage.block_size_read, &read_nb) != FR_OK) LOG_SATA("Can not read %s\n", curBinPath);
      if (read_nb != nb_block_Block*currentImage.block_size_read) LOG_SATA("Bad read %d %d\n", read_nb, nb_block_Block*currentImage.block_size_read);
      last_pos += read_nb;
    }
    usb_cmd_on_going = false;
    blockRequired = false;
    read_done = true;
  }
}

static bool is_3do_iso(uint8_t* buf) {
  uint8_t VOLUME_SYNC_BYTES[] = {0x5A,0x5A,0x5A,0x5A,0x5A};

  if(buf[0] != 0x01)
    return false;

  if(memcmp(&buf[1],&VOLUME_SYNC_BYTES[0],sizeof(VOLUME_SYNC_BYTES)) != 0)
    return false;

  if(buf[6] != 0x01)
    return false;

  return true;
}


static bool is_mode1_2352(uint8_t* buf) {
  uint8_t MODE1_SYNC_PATTERN[] = {0x00,0xFF,0xFF,0xFF, 0xFF,0xFF,0xFF,0xFF, 0xFF,0xFF,0xFF,0x00};
  if(memcmp(&buf[0],&MODE1_SYNC_PATTERN[0],sizeof(MODE1_SYNC_PATTERN))!= 0)
    return false;

  if(buf[0x0F] != 0x01)
    return false;

  return is_3do_iso(&buf[0x10]);
}

static bool isA3doImage(char * path) {
  FIL myFile;
  FRESULT fr;
  uint8_t buf[0x20];
  uint read_nb = 0;
  bool ret = false;
  fr = f_open(&myFile, path, FA_READ);
  if (fr){
    return false;
  }

  if (f_read(&myFile, &buf, 0x20, &read_nb) != FR_OK) {
    f_close(&myFile);
    return false;
  }
  if (read_nb != 0x20)  {
    f_close(&myFile);
    return false;
  }

  ret = ((is_mode1_2352(&buf[0])) || (is_3do_iso(&buf[0])));
  f_close(&myFile);
  return ret;
}

static bool LoadfromCue(char *filePath) {
  FIL myFile;
  FRESULT fr;
  bool  valid = false;
  char line[100];
  unsigned int track_num = 0;
  LOG_SATA("Load From Cue %s\n", filePath);
  fr = f_open(&myFile, filePath, FA_READ);
  if (fr){
     LOG_SATA("can not open %s for reading\n",filePath);
     return false;
  }

  // Time to generate TOC
  for (;;)
  {
    currentImage.nb_track = 0;
    /* Read every line and display it */
    while (f_gets(line, sizeof line, &myFile)) {
      valid = true;
      char line_end[100];
      char line_start[100];
      if (sscanf(line, " %s %[^\r\n]\r\n", line_start, line_end) != EOF) {
        if (strncmp(line_start, "FILE", 4) == 0) {
          FILINFO binInfo;
          char filename[100];
          char *lastDir = rindex(filePath, '\\');
          char * testPath;
          char *newPath = malloc(lastDir - filePath + 1);
          memcpy(newPath, filePath, lastDir - filePath);
          newPath[lastDir-filePath] = 0;

          sscanf(line_end, " \"%[^\"]\"", filename);
          testPath = malloc(strlen(newPath)+strlen(filename)+2);
          sprintf(&testPath[0], "%s\\%s", newPath, filename);
          free(newPath);
          if (curBinPath != NULL) free(curBinPath);
          curBinPath = testPath;
          fr = f_stat(curBinPath, &binInfo);
          if (fr == FR_NO_FILE) {
            free(curBinPath);
            curBinPath = NULL;
            valid = false;
            break; //Bin file does not exists
          }
          if (binInfo.fattrib & AM_DIR) {
            free(curBinPath);
            curBinPath = NULL;
            valid = false;
            break; //not a file
          }
          currentImage.nb_block = binInfo.fsize;
          continue;
        }
        if (strncmp(line_start, "TRACK", 5) == 0) {
          unsigned int sector_size = 0;
          unsigned int ctl_addr = 0;
          if (sscanf(line_end, " %u %[^\r\n]\r\n", &track_num, line_end) != EOF)
          {
            if (strncmp(line_end, "MODE1", 5) == 0)
            {
              // Figure out the track sector size
              if (!isA3doImage(curBinPath)) {
                valid = false;
                break;
              }
              currentImage.block_size =  atoi(line_end + 6);
              currentImage.block_size_read = 2048;
              currentImage.tracks[track_num - 1].CTRL_ADR = 0x4;
              currentImage.tracks[track_num - 1].id = track_num;
              currentImage.tracks[track_num - 1].mode = MODE_1;
            }
            else if (strncmp(line_end, "MODE2", 5) == 0)
            {
              //PhotoCD cue file
              // Figure out the track sector size
              currentImage.block_size = currentImage.block_size_read = atoi(line_end + 6);
              currentImage.block_size_read = 2048;
              currentImage.tracks[track_num - 1].CTRL_ADR = 0x4;
              currentImage.tracks[track_num - 1].id = track_num;
              currentImage.tracks[track_num - 1].mode = MODE_2;
            }
            else if (strncmp(line_end, "AUDIO", 5) == 0)
            {
              // // Update toc entry
              currentImage.block_size = 2352; //(98 * (24))
              currentImage.block_size_read = 2352; // 98*24
              currentImage.tracks[track_num - 1].CTRL_ADR = 0x0;
              currentImage.tracks[track_num - 1].id = track_num;
              currentImage.tracks[track_num - 1].mode = CDDA;
            }
            else {
              valid = false;
              break;
            }
            currentImage.nb_track++;
          } else {
            valid = false;
            break;
          }
          continue;
        }
        else if (strncmp(line_start, "INDEX", 5) == 0)
        {
          unsigned int indexnum, min, sec, frame;
           if (sscanf(line_end, " %u %u:%u:%u\r\n", &indexnum, &min, &sec, &frame) == EOF) {
             valid = false;
             break;
           }

           if (indexnum == 1)
           {
              currentImage.tracks[track_num - 1].msf[0] = min;
              currentImage.tracks[track_num - 1].msf[1] = sec + 2;
              currentImage.tracks[track_num - 1].msf[2] = frame;
              currentImage.tracks[track_num - 1].lba = min*60*75+sec*75+frame;
           }
        }
#ifdef USE_PRE_POST_GAP
        else if (strncmp(line_start, "PREGAP", 6) == 0)
        {
           if (sscanf(line_end, " %d:%d:%d\r\n", &min, &sec, &frame) == EOF)
              break;

           // pregap += MSF_TO_FAD(min, sec, frame);
        }
        else if (strncmp(line_start, "POSTGAP", 7) == 0)
        {
           if (sscanf(line_end, " %d:%d:%d\r\n", &min, &sec, &frame) == EOF)
              break;
        }
#endif
      }
    }
    /* Close the file */
    f_close(&myFile);
    break;
  }
  if (valid) {
    currentImage.first_track = 1;
    currentImage.last_track = currentImage.nb_track;
    currentImage.hasOnlyAudio = true;
    currentImage.format = 0x0;
    for (int i = 0; i<currentImage.last_track; i++)
    {
      if (currentImage.tracks[i].CTRL_ADR & 0x4) {
        currentImage.hasOnlyAudio = false;
      }
      if (currentImage.tracks[i].mode == MODE_2){
        currentImage.format = 0x20; //XA format
      }
    }
    currentImage.nb_block /= currentImage.block_size;
    int lba = currentImage.nb_block + 150;
    currentImage.msf[0] = lba/(60*75);
    lba %= 60*75;
    currentImage.msf[1] = lba / 75;
    currentImage.msf[2] = lba % 75;
  }
  return valid;
}

static bool ValidateInfofromCue(FILINFO *fileInfo) {
  FIL myFile;
  UINT i = strlen(fileInfo->fname);
  FRESULT fr;
  bool  valid = false;
  char line[100];
  unsigned int track_num = 0;
  char *newPath = malloc(strlen(currentImage.curPath)+i+2);
  sprintf(&newPath[0], "%s\\%s", currentImage.curPath, fileInfo->fname);
  fr = f_open(&myFile, newPath, FA_READ);
  if (fr){
     LOG_SATA("can not open %s for reading\n",newPath);
     return false;
  }

  LOG_SATA("Validate Game %s\n", newPath);
  // Time to generate TOC
  for (;;)
  {
    /* Read every line and display it */
    while (f_gets(line, sizeof line, &myFile)) {
      valid = true;
      char line_end[100];
      char line_start[100];
      if (sscanf(line, " %s %[^\r\n]\r\n", line_start, line_end) != EOF) {
        if (strncmp(line_start, "FILE", 4) == 0) {
          FILINFO binInfo;
          char filename[100];
          char * testPath;
          sscanf(line_end, " \"%[^\"]\"", filename);
          testPath = malloc(strlen(currentImage.curPath)+strlen(filename)+2);
          sprintf(&testPath[0], "%s\\%s", currentImage.curPath, filename);
          fr = f_stat(testPath, &binInfo);
          free(testPath);
          if (fr == FR_NO_FILE) {
            valid = false;
            break; //Bin file does not exists
          }
          if (binInfo.fattrib & AM_DIR) {
            valid = false;
            break; //not a file
          }
          // currentImage.nb_block = binInfo.fsize;
          continue;
        }
        if (strncmp(line_start, "TRACK", 5) == 0) {
          unsigned int sector_size = 0;
          unsigned int ctl_addr = 0;
          if (sscanf(line_end, " %u %[^\r\n]\r\n", &track_num, line_end) != EOF)
          {
            if (strncmp(line_end, "MODE1", 5) == 0)
            {
              // Figure out the track sector size
              if (!isA3doImage(curBinPath)) {
                valid = false;
                break;
              }
              // currentImage.block_size =  atoi(line_end + 6);
              // currentImage.block_size_read = 2048;
              // currentImage.tracks[track_num - 1].CTRL_ADR = 0x4;
              // currentImage.tracks[track_num - 1].id = track_num;
              // currentImage.tracks[track_num - 1].mode = MODE_1;
            }
            else if (strncmp(line_end, "MODE2", 5) == 0)
            {
              //PhotoCD cue file
              // Figure out the track sector size
              // currentImage.block_size = currentImage.block_size_read = atoi(line_end + 6);
              // currentImage.block_size_read = 2048;
              // currentImage.tracks[track_num - 1].CTRL_ADR = 0x4;
              // currentImage.tracks[track_num - 1].id = track_num;
              // currentImage.tracks[track_num - 1].mode = MODE_2;
            }
            else if (strncmp(line_end, "AUDIO", 5) == 0)
            {
              // // Update toc entry
              // currentImage.block_size = 2352; //(98 * (24))
              // currentImage.block_size_read = 2352; // 98*24
              // currentImage.tracks[track_num - 1].CTRL_ADR = 0x0;
              // currentImage.tracks[track_num - 1].id = track_num;
              // currentImage.tracks[track_num - 1].mode = CDDA;
            }
            else {
              valid = false;
              break;
            }
            // currentImage.nb_track++;
          } else {
            valid = false;
            break;
          }
          continue;
        }
        else if (strncmp(line_start, "INDEX", 5) == 0)
        {
          unsigned int indexnum, min, sec, frame;
           if (sscanf(line_end, " %u %u:%u:%u\r\n", &indexnum, &min, &sec, &frame) == EOF) {
             valid = false;
             break;
           }

           // if (indexnum == 1)
           // {
           //    currentImage.tracks[track_num - 1].msf[0] = min;
           //    currentImage.tracks[track_num - 1].msf[1] = sec + 2;
           //    currentImage.tracks[track_num - 1].msf[2] = frame;
           //    currentImage.tracks[track_num - 1].lba = min*60*75+sec*75+frame;
           // }
        }
#ifdef USE_PRE_POST_GAP
        else if (strncmp(line_start, "PREGAP", 6) == 0)
        {
           if (sscanf(line_end, " %d:%d:%d\r\n", &min, &sec, &frame) == EOF)
              break;

           // pregap += MSF_TO_FAD(min, sec, frame);
        }
        else if (strncmp(line_start, "POSTGAP", 7) == 0)
        {
           if (sscanf(line_end, " %d:%d:%d\r\n", &min, &sec, &frame) == EOF)
              break;
        }
#endif
      }
    }
    /* Close the file */
    f_close(&myFile);
    break;
  }
  return valid;
}

static bool ValidateInfofromIso(FILINFO *fileInfo) {
  FIL myFile;
  UINT i = strlen(fileInfo->fname);
  FRESULT fr;
  char *newPath = malloc(strlen(currentImage.curPath)+i+2);
  sprintf(&newPath[0], "%s\\%s", currentImage.curPath, fileInfo->fname);
  if (!isA3doImage(newPath)) {
    return false;
  }

  return true;
}

static bool LoadfromIso(char *filePath) {
  FIL myFile;
  FILINFO fileInfo;
  FRESULT fr;
  if (!isA3doImage(filePath)) {
    return false;
  }
  fr = f_stat(filePath, &fileInfo);
  if ((fileInfo.fsize % 2352)==0) {
    currentImage.block_size = 2352;
    currentImage.offset = 16;
  } else if ((fileInfo.fsize % 2048)==0) {
    currentImage.block_size = 2048;
    currentImage.offset = 0;
  } else {
    //Bad format
    LOG_SATA("File is %d bytes length\n", fileInfo.fsize);
    return false;
  }

  LOG_SATA("Load Game %s\n", filePath);

  if (curBinPath != NULL) free(curBinPath);
  curBinPath = filePath;

  currentImage.block_size_read = currentImage.block_size;

  currentImage.nb_block = fileInfo.fsize / currentImage.block_size;

  currentImage.first_track = 1;
  currentImage.last_track = 1;
  currentImage.nb_track = 1;
  currentImage.hasOnlyAudio = false;
  currentImage.format = 0x0; //MODE1 always

  currentImage.tracks[0].CTRL_ADR = 0x4;

  currentImage.tracks[0].msf[0] = 0;
  currentImage.tracks[0].msf[1] = 2;
  currentImage.tracks[0].msf[2] = 0;
  currentImage.tracks[0].lba = 0;
  currentImage.tracks[0].mode = MODE_1;
  currentImage.tracks[0].id = 1;

  int lba = currentImage.nb_block + 150;
  currentImage.msf[0] = lba/(60*75);
  lba %= 60*75;
  currentImage.msf[1] = lba / 75;
  currentImage.msf[2] = lba % 75;

  return true;
}


static bool extractBootImage(FILINFO *fileInfo) {
  FIL myFile;
  UINT i = strlen(fileInfo->fname);
  FRESULT fr;
  char *newPath = malloc(strlen(currentImage.curPath)+i+2);
  sprintf(&newPath[0], "%s\\%s", currentImage.curPath, fileInfo->fname);
  if (!isA3doImage(newPath)) {
    LOG_SATA("Is not a 3DO image %s\n", currentImage.curPath);
    free(newPath);
    return false;
  }
  if ((fileInfo->fsize % 2352)==0) {
    currentImage.block_size = 2352;
    currentImage.offset = 16;
  } else if ((fileInfo->fsize % 2048)==0) {
    currentImage.block_size = 2048;
    currentImage.offset = 0;
  } else {
    //Bad format
    LOG_SATA("File is %d bytes length\n", fileInfo->fsize);
    free(newPath);
    return false;
  }

  if (curBinPath != NULL) free(curBinPath);
  curBinPath = newPath;

  currentImage.block_size_read = currentImage.block_size;

  currentImage.nb_block = fileInfo->fsize / currentImage.block_size;

  currentImage.first_track = 1;
  currentImage.last_track = 1;
  currentImage.nb_track = 1;
  currentImage.hasOnlyAudio = false;
  currentImage.format = 0x0; //MODE1 always

  currentImage.tracks[0].CTRL_ADR = 0x4;

  currentImage.tracks[0].msf[0] = 0;
  currentImage.tracks[0].msf[1] = 2;
  currentImage.tracks[0].msf[2] = 0;
  currentImage.tracks[0].lba = 0;
  currentImage.tracks[0].mode = MODE_1;
  currentImage.tracks[0].id = 1;

  int lba = currentImage.nb_block + 150;
  currentImage.msf[0] = lba/(60*75);
  lba %= 60*75;
  currentImage.msf[1] = lba / 75;
  currentImage.msf[2] = lba % 75;
  return true;
}

static bool validateFile(FILINFO* fileInfo) {
  if (f_path_contains(fileInfo->fname,"*.cue")){
     return ValidateInfofromCue(fileInfo);
  }
  if (f_path_contains(fileInfo->fname,"*.iso")){
    return ValidateInfofromIso(fileInfo);
  }
  return false;
}

static bool loadFile(char* filepath) {
  if (f_path_contains(filepath,"*.cue")){
     return LoadfromCue(filepath);
  }
  if (f_path_contains(filepath,"*.iso")){
    return LoadfromIso(filepath);
  }
  return false;
}

static dir_t* getNewDir(void) {
  dir_t * ret=(dir_t*)malloc(sizeof(dir_t));
  return ret;
}

#define MAX_LEVEL 3
int level = -1;
static bool buildDir(dir_t *dirInfo, char *path) {
  FILINFO fileInfo;
  bool ended = false;
  bool hasGame = false;
  level++;
  LOG_SATA("Explore %s (%d)\n", path, level);
  FRESULT res = f_findfirst(&dirInfo->dir, &fileInfo, path, "*");
  do {
    ended = (strlen(fileInfo.fname) == 0) || (level >= 2) || (res != FR_OK);
    if (!ended) {
      bool isDir = (fileInfo.fattrib & AM_DIR);
      if (isDir && !(fileInfo.fname[0] == '.' || fileInfo.fattrib & (AM_HID | AM_SYS))) {  // skip hidden or system files
        FRESULT res;
        dir_t* new_dir = getNewDir();
        UINT i = strlen(fileInfo.fname);
        char *newPath = malloc(strlen(path)+i+2);
        sprintf(&newPath[0], "%s\\%s", path, fileInfo.fname);
        res = f_opendir(&(new_dir->dir), newPath);
        hasGame |= buildDir(new_dir, newPath);

        f_closedir(&(new_dir->dir));
      } else {
        hasGame |= f_path_contains(fileInfo.fname,"*.cue"); //Verifier le parsing et le cue
        // hasGame |= f_path_contains(fileInfo.fname,"*.bin"); //Verifier le parsing et le cue
        hasGame |= f_path_contains(fileInfo.fname,"*.iso"); //Verifier le parsing et le cue
      }
      res = f_findnext(&dirInfo->dir, &fileInfo);
    }
  } while(!ended);
  level--;
  return hasGame;
}

bool loadPlaylistEntry(uint8_t dev_addr) {
  bool ret = false;
  FRESULT res;
  FILINFO fileInfo;
  LOG_SATA("try to load %d %s\n", playlist.current_entry, playlist.path[playlist.current_entry]);
  if (loadFile(playlist.path[playlist.current_entry])) {
    LOG_SATA("Load entry %d\n",playlist.current_entry);
    playlist.current_entry++;
    if (playlist.current_entry == playlist.nb_entries) {
      LOG_SATA("Playlist done - clearing\n");
      clearPlaylist();
    }

    // memcpy(&currentImage, &allImage[selected_img].info, sizeof(cd_s));
    if (f_open(&curBinFile, curBinPath, FA_READ) == FR_OK) {
      device_s *dev = getDevice(dev_addr);
      LOG_SATA("Game loaded\n");
      last_pos = 0;
      dev->state = MOUNTED;
      usb_cmd_on_going = false;
      currentImage.dev = dev;
      set3doCDReady(currentImage.dev->dev_addr, true);
      set3doDriveMounted(currentImage.dev->dev_addr, true);
      ret = true;
    } else {
      LOG_SATA("Can not open the Game!\n");
    }
  }
  return ret;
}

bool loadBootIso(uint8_t dev_addr) {
  bool ret = false;
  FRESULT res;
  FILINFO fileInfo;
  LOG_SATA("Load boot.iso\n");
  dir_t *curDir = getNewDir();
  char *curPath = (char *) malloc(5);
  snprintf(curPath, 5, "%d:", dev_addr-1);
  LOG_SATA("Try to load on %s\n", curPath);
  res = f_findfirst(&curDir->dir, &fileInfo, curPath, "boot.iso");
  if ((res != FR_OK) || (strlen(fileInfo.fname) == 0)) {
    //report error. Boot iso is not found
    LOG_SATA("Error on %s\n", curPath);
  } else {
    device_s *dev = getDevice(dev_addr);
    currentImage.dev = dev;
    if (currentImage.curDir != NULL) free(currentImage.curDir);
    if (currentImage.curPath != NULL) free(currentImage.curPath);
    currentImage.curDir = curDir;
    currentImage.curPath = curPath;
    //load boot.iso
    if (extractBootImage(&fileInfo)) {
      // memcpy(&currentImage, &allImage[selected_img].info, sizeof(cd_s));
      if (f_open(&curBinFile, curBinPath, FA_READ) == FR_OK) {
        last_pos = 0;
        dev->state = MOUNTED;
        usb_cmd_on_going = false;
        LOG_SATA("Boot iso path %s\n", currentImage.curPath);
        set3doCDReady(currentImage.dev->dev_addr, true);
        set3doDriveMounted(currentImage.dev->dev_addr, true);
        ret = true;
      } else {
        LOG_SATA("Can not open the Game!\n");
      }
    }
    else {
      LOG_SATA("Can not extract on %s\n", curPath);
    }
  }
  if (!ret) {
    free(curDir);
    free(curPath);
    if (currentImage.curDir != NULL) free(currentImage.curDir);
    if (currentImage.curPath != NULL) free(currentImage.curPath);
    currentImage.curDir = NULL;
    currentImage.curPath = NULL;
  }
  return ret;
}

static bool handleBootImage(uint8_t dev_addr) {
  LOG_SATA("Handle Boot image %d (%d)\n", onMountMode, playlist.nb_entries);
  if (playlist.nb_entries != 0) {
    LOG_SATA("Handle playlist\n");
    return loadPlaylistEntry(dev_addr);
  } else {
    LOG_SATA("Handle boot.iso\n");
    return loadBootIso(dev_addr);
  }
}

bool MSC_Inquiry(uint8_t dev_addr, uint8_t lun) {
  FRESULT result;
  LOG_SATA("MSC_Inquiry %d\n", dev_addr);
  if (tuh_msc_get_block_size(dev_addr, lun) == 0) {
    LOG_SATA("MSC block is 0\n");
    return false;
  }
  device_s *dev = getDevice(dev_addr);
  char path[7];
  snprintf(path, 7, "%d://", dev_addr-1);
  LOG_SATA("Try to mount on %s\n", path);
  result = f_mount(&dev->DiskFATState, path , 1);
  if (result!=FR_OK) {
    LOG_SATA("Can not mount\n");
    dev->isFatFs = false;
    dev->useable = false;
    return false;
  }
  dev->isFatFs = true;
  dev->useable = true;
  LOG_SATA("MSC mounted here: %s\n", path);
  mediaInterrupt();
}

static int current_toc = -2;
static int current_toc_level = 0;
static int current_toc_offset = 0;

int getTocLevel(void) {
  return current_toc_level;
}

void setTocLevel(int index) {
  FRESULT res;
  FILINFO fileInfo;
  int curDirNb = 0;
  LOG_SATA("current_toc %d vs %d\n", current_toc, index);

  if (index == -1) {
    LOG_SATA("Set Toc Level start %s level %d\n", currentImage.curPath, current_toc_level);
    if (current_toc_level <= 1) {
      f_closedir(&currentImage.curDir->dir);
      free(currentImage.curPath);
      current_toc_level = 0;
      current_toc = 0;
    } else {
      char *lastDir = rindex(currentImage.curPath, '\\');
      if (lastDir != NULL) {
        bool found = false;
        char *newPath = malloc(lastDir - currentImage.curPath + 1);
        memcpy(newPath, currentImage.curPath, lastDir - currentImage.curPath);
        newPath[lastDir-currentImage.curPath] = 0;
        f_closedir(&currentImage.curDir->dir);
        res = f_opendir(&currentImage.curDir->dir, newPath);
        current_toc_offset = 0;
        free(currentImage.curPath);
        currentImage.curPath = newPath;
        LOG_SATA("Set Toc Level path %s\n", currentImage.curPath);
        current_toc = 0;
        current_toc_level -= 1;
      }
    }
    return;
  }
  if (current_toc != index) {
    //need to change the current TOC level
    //Get required Toc Entry
    if ((current_toc == 0) && (current_toc_level == 0)){
      currentImage.curDir = getNewDir();
      currentImage.curPath = (char*)malloc(20 * sizeof(char));
      snprintf(currentImage.curPath, 20, "%d://", (index>>24)-1);
      res = f_opendir(&currentImage.curDir->dir, currentImage.curPath);
      current_toc_offset = 0;
      current_toc_level++;
      LOG_SATA("Setup root path to %s\n", currentImage.curPath);
    } else {
      int i = 0;
      LOG_SATA("Load path %s\n", currentImage.curPath);
      f_closedir(&currentImage.curDir->dir);
      f_opendir(&currentImage.curDir->dir, currentImage.curPath);
      while(i < index) {
        LOG_SATA("Look for entry %d\n", i);
        if (getNextValidToc(&fileInfo)) {
          i++;
          current_toc_offset++;
        }
        if (fileInfo.fname[0] == 0) {
          //Should raise en arror. Shall never happen
          LOG_SATA("!!! WTF not found\n");
          return; //End of file list
        }
      }
      LOG_SATA("Finish at entry %d\n", i);
      if (fileInfo.fattrib & AM_DIR) {
        int i = strlen(fileInfo.fname);
        char *newPath = malloc(strlen(currentImage.curPath)+i+2);
        sprintf(&newPath[0], "%s\\%s", currentImage.curPath, fileInfo.fname);
        free(currentImage.curPath);
        currentImage.curPath = newPath;
        LOG_SATA("Set Toc Level path  to dir %s\n", currentImage.curPath);
        f_closedir(&currentImage.curDir->dir);
        res = f_opendir(&currentImage.curDir->dir, currentImage.curPath);
        current_toc_offset = 0;
        current_toc_level++;
      } else {
        LOG_SATA("!!! WTF not a directory\n");
      }
    }
    current_toc = index;
  }
}

static bool getNextValidToc(FILINFO *fileInfo) {
  FRESULT res;
  res = f_readdir(&currentImage.curDir->dir, fileInfo);                   /* Read a directory item */
  if (res != FR_OK) fileInfo->fname[0] = 0;  /* Break on error or end of dir */
  // Ne bouger que si dir, iso ou cue, a voir
  if (fileInfo->fname[0] == 0) return false;  /* Break on error or end of dir */
  if ((fileInfo->fname[0] == '.') || (fileInfo->fattrib & (AM_HID | AM_SYS))) return false;
  if (strlen(fileInfo->fname) >= TOC_NAME_LIMIT) return false;
  if (!(fileInfo->fattrib & AM_DIR)) {
    if (!validateFile(fileInfo)) return false;
  }
  LOG_SATA("File %s is ok\n", fileInfo->fname);
  return true;
}

bool seekTocTo(int index) {
  FILINFO fileInfo;
  int i = 0;
  f_closedir(&currentImage.curDir->dir);
  f_opendir(&currentImage.curDir->dir, currentImage.curPath);
  current_toc_offset = 0;
  while(i < index) {
    if (getNextValidToc(&fileInfo)) {
      i++;
      current_toc_offset++;
    }
    if (fileInfo.fname[0] == 0) return false; //End of file list
  }
  return true;
}

bool getReturnTocEntry(toc_entry* toc) {
  if (toc == NULL) return false;
  toc->flags = TOC_FLAG_DIR;
  toc->toc_id = 0xFFFFFFFF;
  toc->name_length = 3;
  toc->name = malloc(toc->name_length);
  snprintf(toc->name, toc->name_length, "..");
  toc->name[2] = 0;
  return true;
}

bool getNextTOCEntry(toc_entry* toc) {
  FILINFO fileInfo;
  if (toc == NULL) return false;

  while (!getNextValidToc(&fileInfo)) {
    if (fileInfo.fname[0] == 0) return false;
  }
  current_toc_offset++;

  if (fileInfo.fattrib & AM_DIR) {                   /* It is a directory */
    toc->flags = TOC_FLAG_DIR;
  } else {                                       /* It is a file. */
    toc->flags = TOC_FLAG_FILE;
  }
  toc->toc_id = current_toc_offset;
  toc->name_length = strlen(fileInfo.fname) + 1;
  toc->name = malloc(toc->name_length);
  snprintf(toc->name, toc->name_length, "%s", fileInfo.fname);
  toc->name[toc->name_length-1] = 0;
  return true;
}

void requestOpenFile(char* name, uint16_t name_length, bool write) {
  uint16_t length = name_length + strlen(currentImage.curPath) + 1;
  if (curFilePath != NULL) free(curFilePath);
  curFilePath = malloc(length);
  curFileMode = write;
  snprintf(&curFilePath[0], length, "%s\\%s", currentImage.curPath, name);
  fileCmdRequired = OPEN;
}

void requestWriteFile(uint8_t* buf, uint16_t length) {
  curBuf = buf;
  curBufLength = length;
  fileCmdRequired = WRITE;
}

void requestReadFile(uint8_t* buf, uint16_t length) {
  curBuf = buf;
  curBufLength = length;
  fileCmdRequired = READ;
}

void requestCloseFile() {
  fileCmdRequired = CLOSE;
}

static void check_open_file() {
  usb_cmd_on_going = true;
  usb_result = false;
  usb_result = (f_open(&curDataFile, curFilePath,  FA_WRITE| FA_READ |FA_OPEN_ALWAYS) == FR_OK);
  usb_cmd_on_going = false;
}

static void check_write_file() {
  usb_cmd_on_going = true;
  uint nb_write;
  usb_result =(f_write(&curDataFile, curBuf, curBufLength, &nb_write) == FR_OK);
  usb_result =(f_sync(&curDataFile) == FR_OK);
  usb_cmd_on_going = false;
}

static void check_read_file() {
  usb_cmd_on_going = true;
  uint nb_read;
  FRESULT res = f_read(&curDataFile, curBuf, curBufLength, &nb_read);
  usb_result = (res == FR_OK);
  if (!usb_result) LOG_SATA("Issue while reading %d (%d) => %x\n", curBufLength, nb_read,res);
  usb_cmd_on_going = false;
}


static void check_close_file() {
  usb_cmd_on_going = true;
  usb_result = (f_close(&curDataFile) == FR_OK);
  usb_cmd_on_going = false;
}

static void check_file(uint8_t dev_addr) {
  switch(fileCmdRequired) {
    case OPEN:
      check_open_file();
      fileCmdRequired = DONE;
    break;
    case WRITE:
      check_write_file();
      fileCmdRequired = DONE;
    break;
    case READ:
      check_read_file();
      fileCmdRequired = DONE;
    break;
    case CLOSE:
      check_close_file();
      fileCmdRequired = DONE;
    break;
    default:
    break;
  }
}

//Need a umount callback

#endif
