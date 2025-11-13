#ifndef FILESYS_INODE_H
#define FILESYS_INODE_H
//（增强型修改）
#include <stdbool.h>
#include "filesys/off_t.h"
#include "devices/block.h"
#include "lib/kernel/list.h"

/*  增强版的inode_disk：其实现了文件空间的可碎片化，允许
  暂未写入的文件空间实现“虚分配”————不占用实际的磁盘空间
    下面介绍一下inode_disk的新布局    */

/*  inode:代表一个文件(仅在内存中实现) 
    inode_disk:代表一块磁盘中的文件扇区(512B) ,其存储在
  磁盘中，文件打开时虚加载到内存中。inode_disk的功能是:
  保存该文件的全部信息，例如组成该文件的所有扇区位置、该
  文件是否是一个目录。
    在inode_disk中，使用group数组记录每个文件空间的扇区
  位置，这保证了文件空间的可碎片化。
    在inode_disk中，使用next_sector分化该文件扇区，组成
  下面的结构：

  [第一块文件扇区]  [之后的文件扇区]
  inode_disk    ---> inode_disk --> inode_disk -->..
  只有第一块文件扇区代表一个文件，之后的文件扇区是为了实现
  “虚分配”的设计策略。
*/

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