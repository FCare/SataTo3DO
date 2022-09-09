#include "3DO.h"
#include "MSC.h"
#include "CDFormat.h"
#include "pico/stdio.h"

#include "diskio.h"


/* -------------------------------------------
- Thanks to https://github.com/trapexit/3dt for 3do image detection and filesystem extraction
---------------------------------------------*/

typedef struct dir_s{
  DIR dir;
  int nbSubDir;
  struct dir_s *subDirs;
} dir_t;

dir_t *curDir = NULL;
char *curPath = NULL;

FIL curReadFile;
FIL curWriteFile;
char *curFilePath = NULL;

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
static bool check_eject();
static bool check_load();
static void check_block();
static void check_file();
static void check_subq();
#endif

static void handleBootImage(void);

static FSIZE_t last_pos = 0;

static FATFS  DiskFATState;
static FIL file;

bool MSC_Host_loop()
{
  #if CFG_TUH_MSC
    if(usb_state & ENUMERATED) {
      if (!(usb_state & COMMAND_ON_GOING)) {
        if (!check_eject()) {
          check_load();
          if (usb_state & DISC_MOUNTED) {
            //Interface shall be generic for command
            check_file();
            check_block();
            check_subq();
            return true;
          } else {
            return false;
          }
        }
      } else {
        check_load();
      }
    }
    return true;
  #endif
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

static bool check_eject() {
  if (requestEject!=-1) {
    //Execute right now
    LOG_SATA("Eject %d\n", requestEject);
    requestEject = -1;
    return true;
  }
  return false;
}

static bool check_load() {
  if (requestLoad!=-1) {
    //Execute right now
    handleBootImage();
    requestLoad = -1;
    return true;
  }
  return false;
}


static void check_subq() {
  if (subqRequired) {
    bool found = false;
    usb_state |= COMMAND_ON_GOING;
    cd_s *target_track = &currentDisc;
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
    usb_state &= ~COMMAND_ON_GOING;
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

static void check_block() {
  if (blockRequired) {
    // printf("Block required %d %d %d %d\n",start_Block, allImage[selected_img].info.tracks[0].lba, nb_block_Block, currentDisc.block_size_read);
    usb_state |= COMMAND_ON_GOING;

    cd_s *target_track = &currentDisc;
    FIL *fileOpen = &curBinFile;
    uint read_nb = 0;
    FSIZE_t offset = (start_Block - target_track->tracks[0].lba)*currentDisc.block_size + currentDisc.offset;
    // printf("Read %lu %lu %lu %lu\n", offset, currentDisc.offset, start_Block,target_track->tracks[0].lba);
    if (currentDisc.block_size != currentDisc.block_size_read) {
      if (currentDisc.format != 0) {
        //Assuming a XA format has only MODE_2 and CDDA track
        if (currentDisc.block_size_read == 2048) {
          //Mode 2 Form1
          offset += 12 + 4 + 8; //Skip Sync, Header and subHeader
        }
      } else {
        //Mode 1 or CDDA
        if (currentDisc.block_size_read == 2048) {
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

          if (f_read(fileOpen, &buffer_Block[data_offset], currentDisc.block_size, &read_nb) != FR_OK) LOG_SATA("Can not read %s\n", curBinPath);
          data_offset += read_nb;
          for (int i = currentDisc.block_size; i < currentDisc.block_size_read; i++) {
            buffer_Block[data_offset++] = 0;
          }
      } else {
        if (f_read(fileOpen, &buffer_Block[0], currentDisc.block_size_read, &read_nb) != FR_OK) LOG_SATA("Can not read %s\n", curBinPath);
      }
    } else {
      if (f_read(fileOpen, buffer_Block, nb_block_Block*currentDisc.block_size_read, &read_nb) != FR_OK) LOG_SATA("Can not read %s\n", curBinPath);
      if (read_nb != nb_block_Block*currentDisc.block_size_read) LOG_SATA("Bad read %d %d\n", read_nb, nb_block_Block*currentDisc.block_size_read);
      last_pos += read_nb;
    }
    usb_state &= ~COMMAND_ON_GOING;
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
    currentDisc.nb_track = 0;
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
          currentDisc.nb_block = binInfo.fsize;
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
              currentDisc.block_size =  atoi(line_end + 6);
              currentDisc.block_size_read = 2048;
              currentDisc.tracks[track_num - 1].CTRL_ADR = 0x4;
              currentDisc.tracks[track_num - 1].id = track_num;
              currentDisc.tracks[track_num - 1].mode = MODE_1;
            }
            else if (strncmp(line_end, "MODE2", 5) == 0)
            {
              //PhotoCD cue file
              // Figure out the track sector size
              currentDisc.block_size = currentDisc.block_size_read = atoi(line_end + 6);
              currentDisc.block_size_read = 2048;
              currentDisc.tracks[track_num - 1].CTRL_ADR = 0x4;
              currentDisc.tracks[track_num - 1].id = track_num;
              currentDisc.tracks[track_num - 1].mode = MODE_2;
            }
            else if (strncmp(line_end, "AUDIO", 5) == 0)
            {
              // // Update toc entry
              currentDisc.block_size = 2352; //(98 * (24))
              currentDisc.block_size_read = 2352; // 98*24
              currentDisc.tracks[track_num - 1].CTRL_ADR = 0x0;
              currentDisc.tracks[track_num - 1].id = track_num;
              currentDisc.tracks[track_num - 1].mode = CDDA;
            }
            else {
              valid = false;
              break;
            }
            currentDisc.nb_track++;
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
              currentDisc.tracks[track_num - 1].msf[0] = min;
              currentDisc.tracks[track_num - 1].msf[1] = sec + 2;
              currentDisc.tracks[track_num - 1].msf[2] = frame;
              currentDisc.tracks[track_num - 1].lba = min*60*75+sec*75+frame;
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
    currentDisc.first_track = 1;
    currentDisc.last_track = currentDisc.nb_track;
    currentDisc.hasOnlyAudio = true;
    currentDisc.format = 0x0;
    for (int i = 0; i<currentDisc.last_track; i++)
    {
      if (currentDisc.tracks[i].CTRL_ADR & 0x4) {
        currentDisc.hasOnlyAudio = false;
      }
      if (currentDisc.tracks[i].mode == MODE_2){
        currentDisc.format = 0x20; //XA format
      }
    }
    currentDisc.nb_block /= currentDisc.block_size;
    int lba = currentDisc.nb_block + 150;
    currentDisc.msf[0] = lba/(60*75);
    lba %= 60*75;
    currentDisc.msf[1] = lba / 75;
    currentDisc.msf[2] = lba % 75;
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
  char *newPath = malloc(strlen(curPath)+i+2);
  sprintf(&newPath[0], "%s\\%s", curPath, fileInfo->fname);
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
          testPath = malloc(strlen(curPath)+strlen(filename)+2);
          sprintf(&testPath[0], "%s\\%s", curPath, filename);
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
          // currentDisc.nb_block = binInfo.fsize;
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
              // currentDisc.block_size =  atoi(line_end + 6);
              // currentDisc.block_size_read = 2048;
              // currentDisc.tracks[track_num - 1].CTRL_ADR = 0x4;
              // currentDisc.tracks[track_num - 1].id = track_num;
              // currentDisc.tracks[track_num - 1].mode = MODE_1;
            }
            else if (strncmp(line_end, "MODE2", 5) == 0)
            {
              //PhotoCD cue file
              // Figure out the track sector size
              // currentDisc.block_size = currentDisc.block_size_read = atoi(line_end + 6);
              // currentDisc.block_size_read = 2048;
              // currentDisc.tracks[track_num - 1].CTRL_ADR = 0x4;
              // currentDisc.tracks[track_num - 1].id = track_num;
              // currentDisc.tracks[track_num - 1].mode = MODE_2;
            }
            else if (strncmp(line_end, "AUDIO", 5) == 0)
            {
              // // Update toc entry
              // currentDisc.block_size = 2352; //(98 * (24))
              // currentDisc.block_size_read = 2352; // 98*24
              // currentDisc.tracks[track_num - 1].CTRL_ADR = 0x0;
              // currentDisc.tracks[track_num - 1].id = track_num;
              // currentDisc.tracks[track_num - 1].mode = CDDA;
            }
            else {
              valid = false;
              break;
            }
            // currentDisc.nb_track++;
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
           //    currentDisc.tracks[track_num - 1].msf[0] = min;
           //    currentDisc.tracks[track_num - 1].msf[1] = sec + 2;
           //    currentDisc.tracks[track_num - 1].msf[2] = frame;
           //    currentDisc.tracks[track_num - 1].lba = min*60*75+sec*75+frame;
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
  char *newPath = malloc(strlen(curPath)+i+2);
  sprintf(&newPath[0], "%s\\%s", curPath, fileInfo->fname);
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
    currentDisc.block_size = 2352;
    currentDisc.offset = 16;
  } else if ((fileInfo.fsize % 2048)==0) {
    currentDisc.block_size = 2048;
    currentDisc.offset = 0;
  } else {
    //Bad format
    LOG_SATA("File is %d bytes length\n", fileInfo.fsize);
    return false;
  }

  LOG_SATA("Load Game %s\n", filePath);

  if (curBinPath != NULL) free(curBinPath);
  curBinPath = filePath;

  currentDisc.block_size_read = currentDisc.block_size;

  currentDisc.nb_block = fileInfo.fsize / currentDisc.block_size;

  currentDisc.first_track = 1;
  currentDisc.last_track = 1;
  currentDisc.nb_track = 1;
  currentDisc.hasOnlyAudio = false;
  currentDisc.format = 0x0; //MODE1 always

  currentDisc.tracks[0].CTRL_ADR = 0x4;

  currentDisc.tracks[0].msf[0] = 0;
  currentDisc.tracks[0].msf[1] = 2;
  currentDisc.tracks[0].msf[2] = 0;
  currentDisc.tracks[0].lba = 0;
  currentDisc.tracks[0].mode = MODE_1;
  currentDisc.tracks[0].id = 1;

  int lba = currentDisc.nb_block + 150;
  currentDisc.msf[0] = lba/(60*75);
  lba %= 60*75;
  currentDisc.msf[1] = lba / 75;
  currentDisc.msf[2] = lba % 75;

  return true;
}


static bool extractBootImage(FILINFO *fileInfo) {
  FIL myFile;
  UINT i = strlen(fileInfo->fname);
  FRESULT fr;
  char *newPath = malloc(strlen(curPath)+i+2);
  sprintf(&newPath[0], "%s\\%s", curPath, fileInfo->fname);
  if (!isA3doImage(newPath)) {
    free(newPath);
    return false;
  }
  if ((fileInfo->fsize % 2352)==0) {
    currentDisc.block_size = 2352;
    currentDisc.offset = 16;
  } else if ((fileInfo->fsize % 2048)==0) {
    currentDisc.block_size = 2048;
    currentDisc.offset = 0;
  } else {
    //Bad format
    LOG_SATA("File is %d bytes length\n", fileInfo->fsize);
    free(newPath);
    return false;
  }

  if (curBinPath != NULL) free(curBinPath);
  curBinPath = newPath;

  currentDisc.block_size_read = currentDisc.block_size;

  currentDisc.nb_block = fileInfo->fsize / currentDisc.block_size;

  currentDisc.first_track = 1;
  currentDisc.last_track = 1;
  currentDisc.nb_track = 1;
  currentDisc.hasOnlyAudio = false;
  currentDisc.format = 0x0; //MODE1 always

  currentDisc.tracks[0].CTRL_ADR = 0x4;

  currentDisc.tracks[0].msf[0] = 0;
  currentDisc.tracks[0].msf[1] = 2;
  currentDisc.tracks[0].msf[2] = 0;
  currentDisc.tracks[0].lba = 0;
  currentDisc.tracks[0].mode = MODE_1;
  currentDisc.tracks[0].id = 1;

  int lba = currentDisc.nb_block + 150;
  currentDisc.msf[0] = lba/(60*75);
  lba %= 60*75;
  currentDisc.msf[1] = lba / 75;
  currentDisc.msf[2] = lba % 75;
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

void loadPlaylistEntry() {
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

    // memcpy(&currentDisc, &allImage[selected_img].info, sizeof(cd_s));
    if (f_open(&curBinFile, curBinPath, FA_READ) == FR_OK) {
      LOG_SATA("Game loaded\n");
      last_pos = 0;
      usb_state |= DISC_MOUNTED;
      usb_state &= ~COMMAND_ON_GOING;
      set3doCDReady(true);
      set3doDriveMounted(true);
    } else {
      LOG_SATA("Can not open the Game!\n");
    }
  }
}

void loadBootIso() {
  FRESULT res;
  FILINFO fileInfo;
  LOG_SATA("Load boot.iso\n");
  curDir = getNewDir();
  curPath = (char *) malloc(3);
  sprintf(curPath, "0:");
  res = f_findfirst(&curDir->dir, &fileInfo, curPath, "boot.iso");
  if ((res != FR_OK) || (strlen(fileInfo.fname) == 0)) {
    //report error. Boot iso is not found
  } else {
    //load boot.iso
    if (extractBootImage(&fileInfo)) {
      // memcpy(&currentDisc, &allImage[selected_img].info, sizeof(cd_s));
      if (f_open(&curBinFile, curBinPath, FA_READ) == FR_OK) {
        last_pos = 0;
        usb_state |= DISC_MOUNTED;
        usb_state &= ~COMMAND_ON_GOING;
        set3doCDReady(true);
        set3doDriveMounted(true);
      } else {
        LOG_SATA("Can not open the Game!\n");
      }
    }
  }
}

static void handleBootImage(void) {
  LOG_SATA("Handle Boot image %d (%d)\n", onMountMode, playlist.nb_entries);
  if (playlist.nb_entries != 0) {
    LOG_SATA("Handle playlist\n");
    loadPlaylistEntry();
  } else {
    LOG_SATA("Handle boot.iso\n");
    loadBootIso();
  }
}

bool MSC_Inquiry(uint8_t dev_addr, msc_cbw_t const* cbw, msc_csw_t const* csw) {
  FRESULT result;
  requestEject = -1;
  LOG_SATA("MSC_Inquiry\n");
  result = f_mount(&DiskFATState, "" , 1);
  if (result!=FR_OK) {
    LOG_SATA("Can not mount\n");
    return false;
  }
  handleBootImage();
  mediaInterrupt();
}

static int current_toc = 0;
static int current_toc_level = 0;
static int current_toc_offset = 0;

int getTocLevel(void) {
  return current_toc_level;
}

void setTocLevel(int index) {
  FRESULT res;
  FILINFO fileInfo;
  int curDirNb = 0;
  if (index == -1) {
    char *lastDir = rindex(curPath, '\\');
    if (lastDir != NULL) {
      bool found = false;
      char *newPath = malloc(lastDir - curPath + 1);
      memcpy(newPath, curPath, lastDir - curPath);
      newPath[lastDir-curPath] = 0;
      f_closedir(&curDir->dir);
      res = f_opendir(&curDir->dir, newPath);
      current_toc_offset = 0;
      free(curPath);
      curPath = newPath;
      current_toc = 0;
      current_toc_level -= 1;
    }
    return;
  }
  if (current_toc != index) {
    //need to change the current TOC level
    //Get required Toc Entry
    int i = 0;
    f_closedir(&curDir->dir);
    f_opendir(&curDir->dir, curPath);
    while(i < index) {
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
    if (fileInfo.fattrib & AM_DIR) {
      int i = strlen(fileInfo.fname);
      char *newPath = malloc(strlen(curPath)+i+2);
      sprintf(&newPath[0], "%s\\%s", curPath, fileInfo.fname);
      free(curPath);
      curPath = newPath;
      f_closedir(&curDir->dir);
      res = f_opendir(&curDir->dir, curPath);
      current_toc_offset = 0;
      current_toc_level++;
    } else {
      LOG_SATA("!!! WTF not a directory\n");
    }
  }
  current_toc = index;
}

static bool getNextValidToc(FILINFO *fileInfo) {
  FRESULT res;
  res = f_readdir(&curDir->dir, fileInfo);                   /* Read a directory item */
  if (res != FR_OK) fileInfo->fname[0] = 0;  /* Break on error or end of dir */
  // Ne bouger que si dir, iso ou cue, a voir
  if (fileInfo->fname[0] == 0) return false;  /* Break on error or end of dir */
  if ((fileInfo->fname[0] == '.') || (fileInfo->fattrib & (AM_HID | AM_SYS))) return false;
  if (strlen(fileInfo->fname) >= TOC_NAME_LIMIT) return false;
  if (!(fileInfo->fattrib & AM_DIR)) {
    if (!validateFile(fileInfo)) return false;
  }
  return true;
}

bool seekTocTo(int index) {
  FILINFO fileInfo;
  int i = 0;
  f_closedir(&curDir->dir);
  f_opendir(&curDir->dir, curPath);
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

void requestOpenFile(char* name, uint16_t name_length) {
  uint16_t length = name_length + strlen(curPath) + 1;
  if (curFilePath != NULL) free(curFilePath);
  curFilePath = malloc(length);
  snprintf(&curFilePath[0], length, "%s\\%s", curPath, name);
  fileCmdRequired = OPEN;
}

void requestWriteFile(uint8_t* buf, uint16_t length) {
  curBuf = buf;
  curBufLength = length;
  fileCmdRequired = WRITE;
}

void requestCloseFile() {
  fileCmdRequired = CLOSE;
}

static void check_open_file() {
  usb_state |= COMMAND_ON_GOING;
  usb_result = false;
  FILINFO fno;
  BYTE mode = FA_WRITE;
  if (f_stat(curFilePath, &fno) != FR_OK) {
    LOG_SATA("File %s does not exist, create it\n", curFilePath);
    mode |= FA_CREATE_NEW;
  }
  usb_result = (f_open(&curWriteFile, curFilePath, mode) == FR_OK);
  usb_state &= ~COMMAND_ON_GOING;
}

static void check_write_file() {
  usb_state |= COMMAND_ON_GOING;
  uint nb_write;
  usb_result =(f_write(&curWriteFile, curBuf, curBufLength, &nb_write) == FR_OK);
  usb_state &= ~COMMAND_ON_GOING;
}

static void check_close_file() {
  usb_state |= COMMAND_ON_GOING;
  f_sync(&curWriteFile);
  f_close(&curWriteFile);
  usb_state &= ~COMMAND_ON_GOING;
}

static void check_file() {
  switch(fileCmdRequired) {
    case OPEN:
      check_open_file();
    break;
    case WRITE:
      check_write_file();
    break;
    case CLOSE:
      check_write_file();
    break;
    default:
    break;
  }
  fileCmdRequired = DONE;
}

//Need a umount callback

#endif
