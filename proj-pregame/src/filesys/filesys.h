#ifndef FILESYS_FILESYS_H
#define FILESYS_FILESYS_H

#include <stdbool.h>
#include "devices/block.h"
#include "filesys/directory.h"
#include "filesys/off_t.h"

/* Sectors of system file inodes. */
#define FREE_MAP_SECTOR 0 /* Free map file inode sector. */
#define ROOT_DIR_SECTOR 1 /* Root directory file inode sector. */

/* Block device that contains the file system. */
//这里的extern：编译阶段相信该文件外有struct block的定义、以及fs_device的实现
//在链接阶段会在其他文件中寻找struct block的定义
extern struct block* fs_device;

enum lookup_mode{
  OPEN,
  REMOVE,
  MKDIR,
  CREATE
};


void filesys_init(bool format);
void filesys_done(void);
bool filesys_create(struct dir* cur_dir, const char* path, off_t initial_size);
struct file* filesys_open(struct dir* cur_dir, const char* path);
bool filesys_remove(struct dir* cur_dir, const char* path);
bool filesys_mkdir(struct dir* cur_dir, const char* path);
bool filesys_cd(struct dir** cur_dir, const char* name);

void filesys_release_lock(void);

#endif /* filesys/filesys.h */
