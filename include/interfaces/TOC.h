#ifndef __TOC_H_INCLUDE__
#define __TOC_H_INCLUDE__

#define TOC_FLAG_FILE      0x1
#define TOC_FLAG_DIR       0x2
#define TOC_FLAG_INVALID   0xffffffff

typedef struct{
    uint32_t flags;
    uint32_t toc_id;  //local toc id
    uint32_t name_length; //strlen
    char* name; //0 terminated
}toc_entry;

#endif