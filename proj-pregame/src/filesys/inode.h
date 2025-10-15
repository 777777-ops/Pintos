#ifndef FILESYS_INODE_H
#define FILESYS_INODE_H

#include <stdbool.h>
#include "filesys/off_t.h"
#include "devices/block.h"
#include "lib/kernel/list.h"

#define GROUPS_MAX_LENGTH 60
#define LAZY_LOAD_LINE 8

/* inode_disk中的成对碎片化空间描述符*/
struct group{
  size_t sectors;
  block_sector_t start;
};

/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk {
  off_t length;         /* File size in bytes. */
  unsigned magic;       /* Magic number. */
  /*  碎片   */
  block_sector_t next_sector;               /* 下一块inode_disk的位置 */
  struct inode_disk* next_inode_disk;       /* 下一块inode_disk在内存中的位置 */
  size_t inner_size;                        /* 本块inode_disk的已分配的扇区个数 */
  size_t group_length;                      /* 碎片化空间描述符数组的末尾索引 */
  struct group groups[GROUPS_MAX_LENGTH];   /* 碎片化空间描述符 */  
  /* 目录 */
  bool is_dir;                             /* 标记是否是目录 */
  size_t dir_entries;
  //unsigned char padding[3];                /* 填充字节，保证结构体大小正好是BLOCK_SECTOR_SIZE */
};

/* In-memory inode. */
struct inode {
  struct list_elem elem;  /* Element in inode list. */
  block_sector_t sector;  /* Sector number of disk location. */
  int open_cnt;           /* Number of openers. */
  bool removed;           /* True if deleted, false otherwise. */
  int deny_write_cnt;     /* 0: writes ok, >0: deny writes. */
  struct inode_disk* data; /* Inode content. */
};

struct bitmap;

void inode_init(void);
bool inode_create(block_sector_t, off_t, bool load, bool is_dir);   /* TODO */
struct inode* inode_open(block_sector_t);  /* TODO */
struct inode* inode_reopen(struct inode*);
block_sector_t inode_get_inumber(const struct inode*);
void inode_close(struct inode*);     /* TODO */
void inode_remove(struct inode*);
off_t inode_read_at(struct inode*, void*, off_t size, off_t offset);          /* TODO */
off_t inode_write_at(struct inode*, const void*, off_t size, off_t offset);   /* TODO */
void inode_deny_write(struct inode*);
void inode_allow_write(struct inode*);
off_t inode_length(const struct inode*);



#endif /* filesys/inode.h */



/* Initializes an inode with LENGTH bytes of data and
   writes the new inode to sector SECTOR on the file system
   device.
   Returns true if successful.
   Returns false if memory or disk allocation fails. 
bool inode_create(block_sector_t sector, off_t length) {
  struct inode_disk* disk_inode = NULL;
  bool success = false;

  ASSERT(length >= 0);

   If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. 
  ASSERT(sizeof *disk_inode == BLOCK_SECTOR_SIZE);

  disk_inode = calloc(1, sizeof *disk_inode);
  if (disk_inode != NULL) {
    size_t sectors = bytes_to_sectors(length);
    disk_inode->length = length;
    disk_inode->magic = INODE_MAGIC;
    if (free_map_allocate(sectors, &disk_inode->start)) {
      block_write(fs_device, sector, disk_inode);
      if (sectors > 0) {
        static char zeros[BLOCK_SECTOR_SIZE];
        size_t i;

        for (i = 0; i < sectors; i++)
          block_write(fs_device, disk_inode->start + i, zeros);
      }
      success = true;
    }
    free(disk_inode);
  }

  return success;
}



*/