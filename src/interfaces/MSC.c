#include "USB.h"
#include "3DO.h"
#include "CDFormat.h"
#include "pico/stdio.h"

#include "diskio.h"

extern uint32_t start_Block;
extern uint32_t nb_block_Block;
extern uint8_t *buffer_Block;
extern bool blockRequired;

extern volatile bool read_done;

#if CFG_TUH_MSC
// static bool check_eject();
// static void check_speed();
static void check_block();
// static bool check_subq();
#endif

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
            // check_subq();
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

static void check_block() {
  if (blockRequired) {
    // printf("Block required %d %d %d %d\n",start_Block, allImage[selected_img].info.tracks[0].lba, nb_block_Block, currentDisc.block_size_read);
    usb_state |= COMMAND_ON_GOING;

    cd_s *target_track = &allImage[selected_img].info;
    FIL *fileOpen = &allImage[selected_img].File;
    uint read_nb = 0;
    uint offset = (start_Block - target_track->tracks[0].lba)*currentDisc.block_size;
    if (currentDisc.block_size != currentDisc.block_size_read) offset += 16; //Skip header
    // printf("Seek %d bytes\n", (start_Block - target_track->tracks[0].lba)*currentDisc.block_size_read);
    if (f_lseek(fileOpen, offset) != FR_OK) printf("Can not seek %s\n", allImage[selected_img].BinPath);
    if (f_read(fileOpen, buffer_Block, nb_block_Block*currentDisc.block_size_read, &read_nb) != FR_OK) printf("Can not read %s\n", allImage[selected_img].BinPath);
    if (read_nb != nb_block_Block*currentDisc.block_size_read) printf("Bad read %d %d\n", read_nb, nb_block_Block*currentDisc.block_size_read);
    // else printf("Read done\n");
    usb_state &= ~COMMAND_ON_GOING;
    blockRequired = false;
    read_done = true;
    //ouvrir le fichier en offset.
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

  printf("Read %s\n", newPath);
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
              allImage[nb_img].info.format = 0x0;
              allImage[nb_img].info.block_size =  atoi(line_end + 6);
              allImage[nb_img].info.block_size_read = 2048;
              allImage[nb_img].info.tracks[track_num - 1].CTRL_ADR = 0x4;
              allImage[nb_img].info.tracks[track_num - 1].id = track_num;
            }
            else if (strncmp(line_end, "MODE1", 5) == 0)
            {
              //PhotoCD cue file
              // Figure out the track sector size
              allImage[nb_img].info.format = 0x20;
              allImage[nb_img].info.block_size = allImage[nb_img].info.block_size_read = atoi(line_end + 6);
              allImage[nb_img].info.block_size_read = 2048;
              allImage[nb_img].info.tracks[track_num - 1].CTRL_ADR = 0x4;
              allImage[nb_img].info.tracks[track_num - 1].id = track_num;
            }
            else if (strncmp(line_end, "AUDIO", 5) == 0)
            {
              // Update toc entry
              allImage[nb_img].info.format = 0x0;
              allImage[nb_img].info.block_size = allImage[nb_img].info.block_size_read = 2352;
              allImage[nb_img].info.tracks[track_num - 1].CTRL_ADR = 0x0;
              allImage[nb_img].info.tracks[track_num - 1].id = track_num;
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
      for (int i = 0; i<allImage[nb_img].info.last_track; i++)
      {
        if (allImage[nb_img].info.tracks[i].CTRL_ADR & 0x4) {
          allImage[nb_img].info.hasOnlyAudio = false;
          break;
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

static void processFile(FILINFO* fileInfo, char* path) {
  if (f_path_contains(fileInfo->fname,"*.cue")){
    printf("CUE FILE %s/%s\n",path, fileInfo->fname);
    ExtractInfofromCue(fileInfo, path);
  }
}

bool MSC_Inquiry(uint8_t dev_addr, msc_cbw_t const* cbw, msc_csw_t const* csw) {
  FRESULT result;
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

  char label[24];
  label[0] = 0;
  f_getlabel("", label, 0);
  printf("Disk label = %s, root directory contains...\n",label);

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
  // selected_img = nb_img-1;
  memcpy(&currentDisc, &allImage[selected_img].info, sizeof(cd_s));
  if (f_open(&allImage[selected_img].File, allImage[selected_img].BinPath, FA_READ) == FR_OK) {
    currentDisc.mounted = true;
    usb_state |= DISC_MOUNTED;
    usb_state &= ~COMMAND_ON_GOING;
    set3doCDReady(true);
    set3doDriveMounted(true);
  }
}

//Need a umount callback

#endif
