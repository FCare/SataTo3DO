#include "USB.h"
#include "3DO.h"
#include "CDFormat.h"
#include "pico/stdio.h"

#include "diskio.h"


/* -------------------------------------------
- Thanks to https://github.com/trapexit/3dt for 3do image detection and filesystem extraction
---------------------------------------------*/


extern uint32_t start_Block;
extern uint32_t nb_block_Block;
extern uint8_t *buffer_Block;
extern bool blockRequired;

extern bool subqRequired;
extern uint8_t *buffer_subq;

extern volatile bool read_done;

#if CFG_TUH_MSC
// static bool check_eject();
// static void check_speed();
static void check_block();
static void check_subq();
#endif

static FSIZE_t last_pos = 0;

static FATFS  DiskFATState;
static FIL file;

bool MSC_Host_loop()
{
  #if CFG_TUH_MSC
    if(usb_state & ENUMERATED) {
      if (!(usb_state & COMMAND_ON_GOING)) {
        // if (!check_eject()) {
          if (usb_state & DISC_MOUNTED) {
            // check_speed();
            check_block();
            check_subq();
            return true;
          } else {
            return false;
          }
        // }
      }
    }
    return true;
  #endif
  }


#if CFG_TUH_MSC

//--------------------------------------------------------------------+
// MACRO TYPEDEF CONSTANT ENUM DECLARATION
//--------------------------------------------------------------------+

volatile bool inquiry_cb_flag;

static bool startClose = true;

uint8_t readBuffer[20480];

static FRESULT processDir(FILINFO* fileInfo, char* path);
static void processFile(FILINFO* fileInfo, char* path);


extern volatile bool is_audio;
extern volatile bool has_subQ;

#define NB_SUPPORTED_GAMES 100

typedef struct {
  char* BinPath; //same number as tracks number
  FIL File;
  cd_s info;
} bin_s;

static bin_s allImage[NB_SUPPORTED_GAMES] = {0};
int nb_img = 0;
int selected_img = 0;

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

static void check_block() {
  if (blockRequired) {
    // printf("Block required %d %d %d %d\n",start_Block, allImage[selected_img].info.tracks[0].lba, nb_block_Block, currentDisc.block_size_read);
    usb_state |= COMMAND_ON_GOING;

    cd_s *target_track = &allImage[selected_img].info;
    FIL *fileOpen = &allImage[selected_img].File;
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
      if (f_lseek(fileOpen, offset) != FR_OK) printf("Can not seek %s\n", allImage[selected_img].BinPath);
    last_pos = offset;
    if (is_audio) {
      if (has_subQ) {
        //Work only for 1 block here
        FSIZE_t data_offset = 0;

          if (f_read(fileOpen, &buffer_Block[data_offset], currentDisc.block_size, &read_nb) != FR_OK) printf("Can not read %s\n", allImage[selected_img].BinPath);
          data_offset += read_nb;
          for (int i = currentDisc.block_size; i < currentDisc.block_size_read; i++) {
            buffer_Block[data_offset++] = 0;
          }
      } else {
        if (f_read(fileOpen, &buffer_Block[0], currentDisc.block_size_read, &read_nb) != FR_OK) printf("Can not read %s\n", allImage[selected_img].BinPath);
      }
    } else {
      if (f_read(fileOpen, buffer_Block, nb_block_Block*currentDisc.block_size_read, &read_nb) != FR_OK) printf("Can not read %s\n", allImage[selected_img].BinPath);
      if (read_nb != nb_block_Block*currentDisc.block_size_read) printf("Bad read %d %d\n", read_nb, nb_block_Block*currentDisc.block_size_read);
      last_pos += read_nb;
    }
    usb_state &= ~COMMAND_ON_GOING;
    blockRequired = false;
    read_done = true;
  }
}

static void processFileorDir(FILINFO* fileInfo) {
  bool isDir = (fileInfo->fattrib & AM_DIR);
  if (!(fileInfo->fname[0] == '.' || fileInfo->fattrib & (AM_HID | AM_SYS))) {  // skip hidden or system files
    if (isDir) processDir(fileInfo, "0:");
    else processFile(fileInfo, "0:");
  }
}

static FRESULT processDir(FILINFO* fileInfo, char *path) {
  FRESULT res;
  DIR dir;
  UINT i;
  FILINFO* fno = malloc(sizeof(FILINFO));

  i = strlen(fileInfo->fname);
  char *newPath = malloc(strlen(path)+i+2);
  sprintf(&newPath[0], "%s\\%s", path, fileInfo->fname);

  res = f_opendir(&dir, fileInfo->fname);                       /* Open the directory */
  if (res == FR_OK) {
    for (;;) {
        res = f_readdir(&dir, fno);                   /* Read a directory item */
        if (res != FR_OK || fno->fname[0] == 0) break;  /* Break on error or end of dir */
        if (fno->fattrib & AM_DIR) {                    /* It is a directory */
            res = processDir(fno, newPath);                    /* Enter the directory */
            if (res != FR_OK) break;
        } else {                                       /* It is a file. */
          processFile(fno, newPath);
        }
    }
    f_closedir(&dir);
  }
  free(newPath);
  return res;
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

static void ExtractInfofromCue(FILINFO *fileInfo, char* path) {
  FIL myFile;
  UINT i = strlen(fileInfo->fname);
  FRESULT fr;
  bool  valid = true;
  char line[100];
  unsigned int track_num = 0;
  char *newPath = malloc(strlen(path)+i+2);
  sprintf(&newPath[0], "%s\\%s", path, fileInfo->fname);
  fr = f_open(&myFile, newPath, FA_READ);
  if (fr){
     printf("can not open %s for reading\n",newPath);
     return;
  }

  printf("Game %d: Read %s\n", nb_img, newPath);
  // Time to generate TOC
  for (;;)
  {
    /* Read every line and display it */
    while (f_gets(line, sizeof line, &myFile)) {
      char line_end[100];
      char line_start[100];
      if (sscanf(line, " %s %[^\r\n]\r\n", line_start, line_end) != EOF) {
        if (strncmp(line_start, "FILE", 4) == 0) {
          FILINFO binInfo;
          char filename[100];
          sscanf(line_end, " \"%[^\"]\"", filename);
          if (allImage[nb_img].BinPath != NULL) free(allImage[nb_img].BinPath);
          allImage[nb_img].BinPath = malloc(strlen(path)+strlen(filename)+2);
          sprintf(&allImage[nb_img].BinPath[0], "%s\\%s", path, filename);
          fr = f_stat(allImage[nb_img].BinPath, &binInfo);
          if (fr == FR_NO_FILE) {
            valid = false;
            break; //Bin file does not exists
          }
          if (binInfo.fattrib & AM_DIR) {
            valid = false;
            break; //not a file
          }
          allImage[nb_img].info.nb_block = binInfo.fsize;
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
              if (!isA3doImage(allImage[nb_img].BinPath)) {
                valid = false;
                break;
              }
              allImage[nb_img].info.block_size =  atoi(line_end + 6);
              allImage[nb_img].info.block_size_read = 2048;
              allImage[nb_img].info.tracks[track_num - 1].CTRL_ADR = 0x4;
              allImage[nb_img].info.tracks[track_num - 1].id = track_num;
              allImage[nb_img].info.tracks[track_num - 1].mode = MODE_1;
            }
            else if (strncmp(line_end, "MODE2", 5) == 0)
            {
              //PhotoCD cue file
              // Figure out the track sector size
              allImage[nb_img].info.block_size = allImage[nb_img].info.block_size_read = atoi(line_end + 6);
              allImage[nb_img].info.block_size_read = 2048;
              allImage[nb_img].info.tracks[track_num - 1].CTRL_ADR = 0x4;
              allImage[nb_img].info.tracks[track_num - 1].id = track_num;
              allImage[nb_img].info.tracks[track_num - 1].mode = MODE_2;
            }
            else if (strncmp(line_end, "AUDIO", 5) == 0)
            {
              // Update toc entry
              allImage[nb_img].info.block_size = 2352; //(98 * (24))
              allImage[nb_img].info.block_size_read = 2352; // 98*24
              allImage[nb_img].info.tracks[track_num - 1].CTRL_ADR = 0x0;
              allImage[nb_img].info.tracks[track_num - 1].id = track_num;
              allImage[nb_img].info.tracks[track_num - 1].mode = CDDA;
            }
            else {
              valid = false;
              break;
            }
            allImage[nb_img].info.nb_track++;
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
              allImage[nb_img].info.tracks[track_num - 1].msf[0] = min;
              allImage[nb_img].info.tracks[track_num - 1].msf[1] = sec + 2;
              allImage[nb_img].info.tracks[track_num - 1].msf[2] = frame;
              allImage[nb_img].info.tracks[track_num - 1].lba = min*60*75+sec*75+frame;
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
    if (valid) {
      allImage[nb_img].info.first_track = 1;
      allImage[nb_img].info.last_track = allImage[nb_img].info.nb_track;
      allImage[nb_img].info.hasOnlyAudio = true;
      allImage[nb_img].info.format = 0x0;
      for (int i = 0; i<allImage[nb_img].info.last_track; i++)
      {
        if (allImage[nb_img].info.tracks[i].CTRL_ADR & 0x4) {
          allImage[nb_img].info.hasOnlyAudio = false;
        }
        if (allImage[nb_img].info.tracks[i].mode == MODE_2){
          allImage[nb_img].info.format = 0x20; //XA format
        }
      }
      allImage[nb_img].info.nb_block /= allImage[nb_img].info.block_size;
      int lba = allImage[nb_img].info.nb_block + 150;
      allImage[nb_img].info.msf[0] = lba/(60*75);
      lba %= 60*75;
      allImage[nb_img].info.msf[1] = lba / 75;
      allImage[nb_img].info.msf[2] = lba % 75;
      nb_img++;
    }

    /* Close the file */
    f_close(&myFile);
    break;
  }
}

static void ExtractInfofromIso(FILINFO *fileInfo, char* path) {
  FIL myFile;
  UINT i = strlen(fileInfo->fname);
  FRESULT fr;
  char *newPath = malloc(strlen(path)+i+2);
  sprintf(&newPath[0], "%s\\%s", path, fileInfo->fname);
  if (!isA3doImage(newPath)) {
    return;
  }
  if ((fileInfo->fsize % 2352)==0) {
    allImage[nb_img].info.block_size = 2352;
    allImage[nb_img].info.offset = 16;
  } else if ((fileInfo->fsize % 2048)==0) {
    allImage[nb_img].info.block_size = 2048;
    allImage[nb_img].info.offset = 0;
  } else {
    //Bad format
    printf("File is %d bytes length\n", fileInfo->fsize);
    free(newPath);
    return;
  }

  printf("Game %d: Read %s\n", nb_img, newPath);

  if (allImage[nb_img].BinPath != NULL) free(allImage[nb_img].BinPath);
  allImage[nb_img].BinPath = newPath;

  allImage[nb_img].info.block_size_read = allImage[nb_img].info.block_size;

  allImage[nb_img].info.nb_block = fileInfo->fsize / allImage[nb_img].info.block_size;

  allImage[nb_img].info.first_track = 1;
  allImage[nb_img].info.last_track = 1;
  allImage[nb_img].info.nb_track = 1;
  allImage[nb_img].info.hasOnlyAudio = false;
  allImage[nb_img].info.format = 0x0; //MODE1 always

  allImage[nb_img].info.tracks[0].CTRL_ADR = 0x4;

  allImage[nb_img].info.tracks[0].msf[0] = 0;
  allImage[nb_img].info.tracks[0].msf[1] = 2;
  allImage[nb_img].info.tracks[0].msf[2] = 0;
  allImage[nb_img].info.tracks[0].lba = 0;
  allImage[nb_img].info.tracks[0].mode = MODE_1;
  allImage[nb_img].info.tracks[0].id = 1;

  int lba = allImage[nb_img].info.nb_block + 150;
  allImage[nb_img].info.msf[0] = lba/(60*75);
  lba %= 60*75;
  allImage[nb_img].info.msf[1] = lba / 75;
  allImage[nb_img].info.msf[2] = lba % 75;
  nb_img++;
}

static void processFile(FILINFO* fileInfo, char* path) {
  if (f_path_contains(fileInfo->fname,"*.cue")){
    ExtractInfofromCue(fileInfo, path);
  }
  if (f_path_contains(fileInfo->fname,"*.iso")){
      ExtractInfofromIso(fileInfo, path);
  }
}

bool MSC_Inquiry(uint8_t dev_addr, msc_cbw_t const* cbw, msc_csw_t const* csw) {
  FRESULT result;
  requestEject = -1;
  printf("MSC_Inquiry\n");
  for (int i = 0; i<nb_img; i++){
    for (int j=0; j<100; j++) {
      if (allImage[i].BinPath != NULL) {
        free(allImage[i].BinPath);
        allImage[i].BinPath = NULL;
      }
    }
  }
  nb_img = 0;
  result = f_mount(&DiskFATState, "" , 1);
  if (result!=FR_OK) return false;

  printf("root directory contains...\n");

  static FILINFO fileInfo;
  DIR dirInfo;

  FRESULT res = f_findfirst(&dirInfo, &fileInfo, "", "*");
  processFileorDir(&fileInfo);
  while (1) {
    res = f_findnext(&dirInfo, &fileInfo);
    if (res != FR_OK || fileInfo.fname[0] == 0) {
      break;
    }
    processFileorDir(&fileInfo);
  }
  selected_img = 19;
  memcpy(&currentDisc, &allImage[selected_img].info, sizeof(cd_s));
  if (f_open(&allImage[selected_img].File, allImage[selected_img].BinPath, FA_READ) == FR_OK) {
    last_pos = 0;
    currentDisc.mounted = true;
    usb_state |= DISC_MOUNTED;
    usb_state &= ~COMMAND_ON_GOING;
    set3doCDReady(true);
    set3doDriveMounted(true);
  } else {
    printf("Can not open the Game!\n");
  }
}

//Need a umount callback

#endif
