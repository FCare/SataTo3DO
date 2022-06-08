#include "USB.h"
#include "3DO.h"
#include "CDFormat.h"
#include "pico/stdio.h"

#include "diskio.h"

#if CFG_TUH_MSC
static bool check_eject();
static void check_speed();
static void check_block();
static void check_mount();
static bool check_subq();
#endif

static FATFS  DiskFATState;
static FIL file;

void MSC_Host_loop()
{
#if CFG_TUH_MSC
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
  char line[100];
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
        printf("%s\n", line);
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

  FATFS* ff;
  DWORD space;
  UINT  quantity;
  result = f_getfree("", &space, &ff);
  char  buffer[] = "hello new file";
  printf("Get free space on disk (err=%d) in sectors %d\n", result, space);
  printf("Writing a file...\n");
  result = f_open(&file, "test-file.txt", FA_CREATE_ALWAYS | FA_WRITE);
  printf("opening test-file.txt (err=%d)\n", result);
  print_error_text(result);
  result = f_write(&file, buffer, sizeof(buffer), &quantity);
  printf("writing (err=%d) with %d bytes written\n", result, quantity);
  print_error_text(result);
  result =  f_close(&file);
  printf("closing (err=%d)\n", result);
  print_error_text(result);
}

#endif
