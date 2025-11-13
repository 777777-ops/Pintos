//（增强性修改）
#include "stdbool.h"
#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"
#include "threads/synch.h"

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44

static void inode_release_inner(struct inode*, bool); 
static struct inode_disk* inode_end_inner(struct inode*);
static bool inode_disk_lazy_alloc(struct inode_disk*);
static bool inode_write_expand(struct inode* inode, off_t size, off_t offset);
static bool inode_allocate_sectors(size_t, struct inode*);
static struct inode_disk* inode_create_inner(size_t, struct inode*, block_sector_t*);
static void inode_release_sectors(struct inode*);

/* Returns the number of sectors to allocate for an inode SIZE
   bytes long. */
static inline size_t bytes_to_sectors(off_t size) { return DIV_ROUND_UP(size, BLOCK_SECTOR_SIZE); }

/*根据新设定的inode布局，在单个文件空间中定位扇区的的算法有所改变：
    1.先遍历到pos所属的inode_disk(管理块)中
    2.再利用管理块中的所记录的sector扇区信息遍历到pos指定扇区 */
static block_sector_t byte_to_sector(const struct inode* inode, off_t pos) {
  ASSERT(inode != NULL);
  if (pos < inode->data->length){
    size_t last_cnt = pos / BLOCK_SECTOR_SIZE;
    struct inode_disk* i_d = inode->data;
    /* 锁定到pos的inode_disk */
    while(i_d != NULL){
      if(i_d->inner_size <= last_cnt){
        last_cnt -= i_d->inner_size;
        i_d = i_d->next_inode_disk;
      }else
        break;
    }
    if(i_d == NULL)
      PANIC(" NO WAY");

    /* 锁定到pos对应的sector上*/
    for(size_t i = 0; i < i_d->group_length; i++){
      if(i_d->groups[i].sectors <= last_cnt){
        last_cnt -= i_d->groups[i].sectors;
        continue;
      }
      /* 锁定到了pos对应的连续碎片上 */
      if(i_d->groups[i].start == 0){
        /* 虚分配要初始化并且返回 */
        if(inode_disk_lazy_alloc(i_d)){
          ASSERT(i_d->groups[i].start > 0)
          if(i_d->groups[i].sectors <= last_cnt){
            last_cnt -= i_d->groups[i].sectors;
            continue;
          }else
            return i_d->groups[i].start + last_cnt;  
        }else      
          PANIC(" DEBUG ");
      }else
        return i_d->groups[i].start + last_cnt;  

    }
    PANIC(" NO WAY");
    return -1;
  }
  else
    return -1;

}

/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;

/* Initializes the inode module. */
void inode_init(void) { list_init(&open_inodes); }

/*  需提供一个空闲扇区SECTOR作为该文件的管理块，LOAD标志该文件是否需要实
  加载，IS_DIR标志该新建的文件是否是一个目录文件   */
bool inode_create(block_sector_t sector, off_t length, bool load, bool is_dir) {
  struct inode_disk* disk_inode = NULL;

  ASSERT(length >= 0);

  size_t cnt = DIV_ROUND_UP(length, BLOCK_SECTOR_SIZE);
  /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */
  ASSERT(sizeof *disk_inode == BLOCK_SECTOR_SIZE);

  disk_inode = calloc(1, sizeof *disk_inode);
  if (disk_inode != NULL) {
    disk_inode->magic = INODE_MAGIC;
    bool success = false;
    struct inode* inode = calloc(1, sizeof(struct inode));
    if(inode == NULL)
      return false;

    inode->data = disk_inode;
    disk_inode->length = length;      
    /* 实加载 */
    if(load)
      success = inode_allocate_sectors(cnt, inode);
    /* 虚分配 */
    else{
      disk_inode->groups[0].sectors = cnt;
      disk_inode->groups[0].start = 0;
      disk_inode->inner_size = cnt;
      disk_inode->group_length++;
      success = true;
    }
    disk_inode->is_dir = is_dir;

    free(inode);
    block_write(fs_device, sector, disk_inode);
    free(disk_inode);
    return success;
  }
  return false;
}

/*  根据提供的管理扇区SECTOR打开一个文件，为适应新文件布局，
  对本函数做了以下增强：
    遍历并且打开所有的inode_disk管理块 */
struct inode* inode_open(block_sector_t sector) {
  struct list_elem* e;
  struct inode* inode;

  /* Check whether this inode is already open. */
  for (e = list_begin(&open_inodes); e != list_end(&open_inodes); e = list_next(e)) {
    inode = list_entry(e, struct inode, elem);
    if (inode->sector == sector) {
      inode_reopen(inode);
      return inode;
    }
  }

  /* Allocate memory. */
  inode = malloc(sizeof *inode);
  if (inode == NULL){
    return NULL;
  }

  /* Initialize. */
  list_push_front(&open_inodes, &inode->elem);
  inode->sector = sector;
  inode->open_cnt = 1;
  inode->deny_write_cnt = 0;
  inode->removed = false;

  /* 初始化第一个inode_disk */
  struct inode_disk* i_d = malloc(sizeof(struct inode_disk));
  struct inode_disk* i_d_next;
  if(i_d == NULL)
    return NULL;
  block_read(fs_device, inode->sector, i_d);
  inode->data = i_d;
  i_d->next_inode_disk = NULL;

  /* 初始化后续的inode_disk */
  while(i_d->next_sector != 0){
    i_d_next = malloc(sizeof(struct inode_disk));
    if(i_d_next == NULL){
      inode_release_inner(inode, false);         /* 释放已经打开的页 */
      return NULL;
    }
    block_read(fs_device, i_d->next_sector, i_d_next);
    i_d_next->next_inode_disk = NULL;

    /* 继续遍历 */
    i_d->next_inode_disk = i_d_next;
    i_d = i_d_next;
  }
  i_d->next_inode_disk = NULL;

  return inode;
}

/* Reopens and returns INODE. */
struct inode* inode_reopen(struct inode* inode) {

  if (inode != NULL)
    inode->open_cnt++;

  return inode;
}

/* Returns INODE's inode number. */
block_sector_t inode_get_inumber(const struct inode* inode) { return inode->sector; }

/* Closes INODE and writes it to disk.
   If this was the last reference to INODE, frees its memory.
   If INODE was also a removed inode, frees its blocks. */
void inode_close(struct inode* inode) {
  /* Ignore null pointer. */
  if (inode == NULL)
    return;

  /* Release resources if this was the last opener. */
  if (--inode->open_cnt == 0) {
    /* Remove from inode list and release lock. */
    list_remove(&inode->elem);

    /* Deallocate blocks if removed. */
    if (inode->removed) 
      inode_release_sectors(inode);

    /* 如果要删除就不写回，不删除就写回*/
    inode_release_inner(inode, !inode->removed);
    free(inode);
  }

}

/* Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void inode_remove(struct inode* inode) {
  ASSERT(inode != NULL);
  inode->removed = true;
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t inode_read_at(struct inode* inode, void* buffer_, off_t size, off_t offset) {

  uint8_t* buffer = buffer_;
  off_t bytes_read = 0;
  uint8_t* bounce = NULL;

  if(offset >= inode->data->length)
    return 0;
  while (size > 0) {
    /* Disk sector to read, starting byte offset within sector. */
    block_sector_t sector_idx = byte_to_sector(inode, offset);
    int sector_ofs = offset % BLOCK_SECTOR_SIZE;

    /* Bytes left in inode, bytes left in sector, lesser of the two. */
    off_t inode_left = inode_length(inode) - offset;
    int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
    int min_left = inode_left < sector_left ? inode_left : sector_left;

    /* Number of bytes to actually copy out of this sector. */
    int chunk_size = size < min_left ? size : min_left;
    if (chunk_size <= 0)
      break;

    if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE) {
      /* Read full sector directly into caller's buffer. */
      block_read(fs_device, sector_idx, buffer + bytes_read);
    } else {
      /* Read sector into bounce buffer, then partially copy
             into caller's buffer. */
      if (bounce == NULL) {
        bounce = malloc(BLOCK_SECTOR_SIZE);
        if (bounce == NULL)
          break;
      }
      block_read(fs_device, sector_idx, bounce);
      memcpy(buffer + bytes_read, bounce + sector_ofs, chunk_size);
    }

    /* Advance. */
    size -= chunk_size;
    offset += chunk_size;
    bytes_read += chunk_size;
  }
  free(bounce);

  return bytes_read;
}

/* 写入之前，如果写入的起始偏移量大于文件末尾4KB以上要实现虚分配，
  如果写入的末尾偏移量大于文件末尾要实现实分配             
  i_d_old 是原inode_disk链表中的最后一个inode_disk节点
  i_d_lazy 是新创建的虚分配inode_disk节点
  i_d_load 是新创建的实分配inode_disk节点
  
  如果在虚分配阶段失败，无需释放任何资源
  如果i_d_load节点创建，但是实分配失败，就释放i_d_lazy和i_d_load
  如果i_d_load节点创建失败只需释放i_d_lazy                     */
static bool inode_write_expand(struct inode* inode, off_t size, off_t offset){
    struct inode_disk* i_d = inode->data;
    off_t length_saved = i_d->length;
    struct inode_disk* i_d_end = inode_end_inner(inode);     /* 用于分配失败时释放的索引 */
    struct inode_disk* i_d_lazy;
    struct inode_disk* i_d_load;

    block_sector_t lazy_sector = 0;
    block_sector_t load_sector = 0;

    bool empty = i_d_end->inner_size == 0;
    size_t expand_sectors = (size_t)(offset - i_d->length)/BLOCK_SECTOR_SIZE;
    bool lazy = expand_sectors >= LAZY_LOAD_LINE;
    bool had_lazy = i_d_end->groups[0].start == 0 && !empty;

    /* 写入的起始位置超过原文件大小过多选择虚分配  虚分配需要新创建一个i_d*/
    if(lazy){
      i_d_lazy = inode_create_inner(expand_sectors, inode, &lazy_sector);
      if(i_d_lazy == NULL) 
        return false;
      i_d_lazy->groups[i_d_lazy->group_length].sectors = expand_sectors;
      i_d_lazy->groups[i_d_lazy->group_length].start = 0; 
      i_d_lazy->group_length++;
      
      i_d->length += expand_sectors * BLOCK_SECTOR_SIZE;
    }

    /* 如果没有需要新加载的SECTOR，也就是说需要拓展的大小没有超过原SECTOR的末尾
      直接修改长度即可（这种情况此前一定不会触发lazy虚加载）        */
    expand_sectors = DIV_ROUND_UP(offset + size, BLOCK_SECTOR_SIZE) - DIV_ROUND_UP(i_d->length, BLOCK_SECTOR_SIZE);
    if(expand_sectors == 0){
      i_d->length = offset + size;
      return true;
    }
    
    /* 写入的结束位置到当前文件位置需要填充 ，实加载的i_d不能和虚分配的i_d相混，
      如果最后一个i_d是虚分配空间，那么就要新创建一个i_d */
    if(lazy || had_lazy){
      i_d_load = inode_create_inner(0, inode, &load_sector);
      if(i_d_load == NULL)
        goto error;
    }
    if(!inode_allocate_sectors(expand_sectors, inode))
      goto error;
    
    i_d->length = offset + size;
    return true;

error:
    if(lazy_sector > 0){
      free_map_release(lazy_sector, 1);
      free(i_d_lazy);
    }
    if(load_sector > 0){
      free_map_release(load_sector, 1);
      free(i_d_load);
    }
    i_d_end->next_inode_disk = NULL;
    i_d_end->next_sector = 0;
    if(empty){
      memset(i_d_end, 0, sizeof(struct inode_disk));
      i_d_end->magic = INODE_MAGIC;
    }
    i_d->length = length_saved;
    return false;
}



/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if end of file is reached or an error occurs.
   (Normally a write at end of file would extend the inode, but
   growth is not yet implemented.) */
off_t inode_write_at(struct inode* inode, const void* buffer_, off_t size, off_t offset) {

  const uint8_t* buffer = buffer_;
  off_t bytes_written = 0;
  uint8_t* bounce = NULL;

  if (inode->deny_write_cnt || size <= 0)
    return 0;

  struct inode_disk* i_d = inode->data;
  ASSERT(i_d != NULL);
  

  /* 文件拓展 */
  if(offset + size > i_d->length)
    if(!inode_write_expand(inode, size, offset))
      return -1;

  while (size > 0) {
    /* Sector to write, starting byte offset within sector. */
    block_sector_t sector_idx = byte_to_sector(inode, offset);
    int sector_ofs = offset % BLOCK_SECTOR_SIZE;

    /* Bytes left in inode, bytes left in sector, lesser of the two. */
    off_t inode_left = inode_length(inode) - offset;
    int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
    int min_left = inode_left < sector_left ? inode_left : sector_left;

    /* Number of bytes to actually write into this sector. */
    int chunk_size = size < min_left ? size : min_left;
    if (chunk_size <= 0)
      break;

    if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE) {
      /* Write full sector directly to disk. */
      block_write(fs_device, sector_idx, buffer + bytes_written);
    } else {
      /* We need a bounce buffer. */
      if (bounce == NULL) {
        bounce = malloc(BLOCK_SECTOR_SIZE);
        if (bounce == NULL)
          break;
      }

      /* If the sector contains data before or after the chunk
             we're writing, then we need to read in the sector
             first.  Otherwise we start with a sector of all zeros. */
      if (sector_ofs > 0 || chunk_size < sector_left)
        block_read(fs_device, sector_idx, bounce);
      else
        memset(bounce, 0, BLOCK_SECTOR_SIZE);
      memcpy(bounce + sector_ofs, buffer + bytes_written, chunk_size);
      block_write(fs_device, sector_idx, bounce);
    }

    /* Advance. */
    size -= chunk_size;
    offset += chunk_size;
    bytes_written += chunk_size;
  }
  free(bounce);

  return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
void inode_deny_write(struct inode* inode) {
  inode->deny_write_cnt++;
  ASSERT(inode->deny_write_cnt <= inode->open_cnt);
}

/* Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void inode_allow_write(struct inode* inode) {
  ASSERT(inode->deny_write_cnt > 0);
  ASSERT(inode->deny_write_cnt <= inode->open_cnt);
  inode->deny_write_cnt--;
}

/* Returns the length, in bytes, of INODE's data. */
off_t inode_length(const struct inode* inode) { return inode->data->length; }




/* 在inode中分配一个新的inode_disk 
  这里CNT只有在虚分配时需要传递，实加载不需要传递CNT */
static struct inode_disk* inode_create_inner(size_t cnt, struct inode* i, block_sector_t* sector){
  struct inode_disk* i_d = inode_end_inner(i);
  *sector = 0;
  ASSERT(i_d != NULL);
  ASSERT(i_d->group_length < GROUPS_MAX_LENGTH);

  /* 如果最后一个i_d本身就是空的，返回自身 */
  if(i_d->inner_size == 0){
    i_d->inner_size = cnt;
    return i_d;
  }
  
  /* 从磁盘中新分配一个i_d */
  if(!free_map_allocate(1, sector))
    return NULL;
  struct inode_disk* inode_disk = calloc(1, sizeof(struct inode_disk));
  if(inode_disk){
    i_d->next_sector = *sector;
    i_d->next_inode_disk = inode_disk;
    inode_disk->inner_size = cnt;
    inode_disk->is_dir = false;
    return inode_disk;
  }
  free_map_release(*sector, 1);
  *sector = 0;
  return NULL;
  
}


/* 在inode中的最后一个inode_disk里分配CNT个扇区 */
static bool inode_allocate_sectors(size_t cnt, struct inode* i){
  struct inode_disk* i_d = inode_end_inner(i);
  ASSERT(i_d != NULL);
  ASSERT(i_d->group_length < GROUPS_MAX_LENGTH);

  static char zeros[BLOCK_SECTOR_SIZE];
  block_sector_t *start = &i_d->groups[i_d->group_length].start;
  if(cnt == 0)
    return true;
  /* 分配连续空间*/
  if(free_map_allocate(cnt, start)){
    for(size_t i = 0; i < cnt; i++)
      block_write(fs_device, *start + i, zeros);
    
    /* group合并优化 */
    if(i_d->group_length > 0 && 
      i_d->groups[i_d->group_length - 1].start + i_d->groups[i_d->group_length - 1].sectors == *start){

      i_d->groups[i_d->group_length - 1].sectors += cnt;
      *start = 0;
    }
    else{
      i_d->groups[i_d->group_length].sectors = cnt;
      i_d->group_length ++;
    }
    i_d->inner_size += cnt;
    return true;
  }

  size_t old_length = i_d->group_length;
  size_t cnt_last = cnt;
  size_t alloc_cnt = 0;
  while((alloc_cnt = free_map_allocate_longest(start)) > 0){
    for(size_t i = 0; i < alloc_cnt; i++)
      block_write(fs_device, *start + i, zeros);
    i_d->groups[i_d->group_length].sectors = alloc_cnt;
    i_d->group_length++;
    start = &i_d->groups[i_d->group_length].start;
    cnt_last -= alloc_cnt;

    /* 削减了cnt_last后，要考虑连续分配 */
    ASSERT(i_d->group_length < GROUPS_MAX_LENGTH);
    if(free_map_allocate(cnt_last, start)){
      for(size_t i = 0; i < cnt_last; i++)
        block_write(fs_device, *start + i, zeros);
      i_d->groups[i_d->group_length].sectors = cnt_last;
      i_d->group_length ++;
      cnt_last = 0;
      break;
    }
  }
  /* 分配失败，可能是因为扇区空间已满，需要释放资源 */
  if(cnt_last > 0){
    for(size_t i = old_length; i < i_d->group_length; i++){
      free_map_release(i_d->groups[i].start, i_d->groups[i].sectors);
      i_d->groups[i].sectors = 0;
      i_d->groups[i].start = 0;
    }
    i_d->group_length = old_length;
    return false;
  }
  i_d->inner_size += cnt;
  return true;
}


/* 在虚分配（懒分配）的inode_disk中实分配扇区 要写入全0 */
static bool inode_disk_lazy_alloc(struct inode_disk* i_d){
  ASSERT(i_d != NULL);
  ASSERT(i_d->group_length == 1);
  static char zeros[BLOCK_SECTOR_SIZE];

  i_d->group_length = 0;
  block_sector_t *start = &i_d->groups[0].start;
  size_t cnt = i_d->groups[0].sectors;
  /* 分配连续空间 */
  if(free_map_allocate(cnt, start)){
    for(block_sector_t i = 0; i < cnt; i++)
      block_write(fs_device, i + *start, zeros);
    i_d->group_length++;
    return true;
  }
  
  /* 分配碎片空间 */
  size_t alloc_cnt = 0;
  size_t cnt_last = cnt;
  while((alloc_cnt = free_map_allocate_longest(start)) > 0){
    for(block_sector_t i = 0; i < alloc_cnt; i++)
      block_write(fs_device, i + *start, zeros);
    i_d->groups[i_d->group_length].sectors = alloc_cnt;
    i_d->group_length++;
    start = &i_d->groups[i_d->group_length].start;
    cnt_last -= alloc_cnt;

    /* 削减了cnt_last后，要考虑连续分配 */
    ASSERT(i_d->group_length < GROUPS_MAX_LENGTH);
    if(free_map_allocate(cnt_last, start)){
      for(block_sector_t i = 0; i < cnt_last; i++)
        block_write(fs_device, i + *start, zeros);
      i_d->groups[i_d->group_length].sectors = cnt_last;
      i_d->group_length ++;
      cnt_last = 0;
      break;
    }
  }
  /* 分配失败 */
  if(cnt_last > 0){
    for(size_t i = 0; i < i_d->group_length; i++){
      free_map_release(i_d->groups[i].start, i_d->groups[i].sectors);
      i_d->groups[i].sectors = 0;
      i_d->groups[i].start = 0;
    }
    i_d->group_length = 0;
    i_d->groups[0].sectors = cnt;
    i_d->groups[0].start = 0;
    return false;
  }
  /* 可以考虑后项合并 */   /* TODO */
  return true;
}



/* 释放inode中的所有扇区（磁盘清理）*/
static void inode_release_sectors(struct inode* i){
  struct inode_disk* i_d = i->data;
  block_sector_t i_d_sector = i->sector;
  struct group* group;
  while(i_d != NULL){
    /* 清理inode_disk中记录的所有碎片扇区 */
    for(size_t i = 0; i < i_d->group_length; i++){
      group = &i_d->groups[i];
      if(group->start > 0)
        free_map_release(group->start, group->sectors);
    }
    /* 清理inode_disk */
    free_map_release(i_d_sector, 1);
    i_d_sector = i_d->next_sector;
    i_d = i_d->next_inode_disk;
  }
}


/* 释放一个inode中的一整片inode_disk（内存清理） */
static void inode_release_inner(struct inode* inode, bool write_back){

  block_sector_t sector_cur = inode->sector;
  struct inode_disk* i_d_cur = inode->data;
  struct inode_disk* i_d_free;

  while(i_d_cur != NULL){
    i_d_free = i_d_cur;

    /* 写回前一定要清空原来的指向下一个inode_disk的指针 */
    struct inode_disk* next = i_d_cur->next_inode_disk;
    i_d_cur->next_inode_disk = NULL;
    if(write_back)
      block_write(fs_device, sector_cur, i_d_cur);  
  
    sector_cur = i_d_cur->next_sector;
    i_d_cur = next;

    free(i_d_free);
  }
} 




/* 找到inode中的最后一个inode_disk*/
static struct inode_disk* inode_end_inner(struct inode* i){
  struct inode_disk* i_d = i->data;
  while(i_d->next_inode_disk != NULL)
    i_d = i_d->next_inode_disk;
  return i_d;
}

