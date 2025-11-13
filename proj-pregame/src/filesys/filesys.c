//(原创)：重写了所有filesys功能函数
#include "filesys/filesys.h"
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "threads/synch.h"
#include "threads/malloc.h"
#include "threads/thread.h"
#include "userprog/process.h"
#include "filesys/file.h"
#include "filesys/free-map.h"
#include "filesys/inode.h"

/*笔者对原框架提供的文件系统所有函数都进行了重写，下面指出原框架文件系统和增强版的不同*/
/*
  原文件系统：
    1.仅支持在根目录下操作文件
    2.不允许目录之间的跳转，本身也就一个目录
    
  新文件系统：
    1.允许目录之间的嵌套，不再局限于根目录
    2.每个目录中新增了"."和".."两个文件项，允许返回
    上一级
    3.每个进程维护自身的工作目录，文件系统通过路径的
    首字符是否为'/'判断路径是相对还是绝对路径
    4.实现了文件系统同步机制
  
  总结：原框架文件系统可操作性十分有限，仅能维护根目录，
  而在本代码中，重塑了一整套文件系统操作，使其能实现类
  Unix的文件操作
*/    

/*下面指出所有函数的实现思路：
    1.将进程提供的path提交给filesys_lookup()函数，在
    函数中会解析路径，返回最后一级文件名(也就是所操作的
    目标文件)以及目标文件所属的目录的磁盘扇区inode。

    2.根据filesys_lookup()返回的两个参数足以完成目录
    与文件间的常规操作。

    3.代码实现方面，几乎所有的filesys_xxx()函数都采用
    done的单标签跳转编程思路，这是因为文件系统操作总会
    新建许多需要释放的空间资源
*/

/* Partition that contains the file system. */
struct block* fs_device;

/* 临时锁 */
struct lock temporary;

static void do_format(void);
static char** parse_path(const char* path, int* count);
static void free_path_components(char** components, int count);
static bool filesys_dir_create(struct inode* parent, block_sector_t allocated, const char* name);
static char* filesys_lookup(struct dir* cur_dir, const char *path, struct inode** inode);

/* 初始化文件夹系统 */
void filesys_init(bool format) {
  fs_device = block_get_role(BLOCK_FILESYS);
  if (fs_device == NULL)
    PANIC("No file system device found, can't initialize file system.");

  lock_init(&temporary);
  inode_init();
  free_map_init();

  if (format)
    do_format();

  free_map_open();

#ifdef USERPROG
  thread_current()->pcb->dir = dir_open_root();
#endif
}

/* Shuts down the file system module, writing any unwritten data
   to disk. */
void filesys_done(void) { free_map_close(); }

/* 新建文件 */
bool filesys_create(struct dir* cur_dir, const char* path, off_t initial_size) {
  lock_acquire(&temporary);

  struct inode* inode;
  bool success = false;
  char *name = NULL;
  struct dir * dir = NULL;
  block_sector_t inode_sector = 0;
  bool had_add = false;

  if((name = filesys_lookup(dir_reopen(cur_dir), path, &inode)) == NULL)
    goto done;

  dir = dir_open(inode);
  /* 创建文件 */
  if(!free_map_allocate(1, &inode_sector))
    goto done;
  had_add = dir_add(dir, name, inode_sector);
  if(!had_add)
    goto done;
  success = inode_create(inode_sector, initial_size, true, false);

done:
  if(inode_sector > 0 && !success)
    free_map_release(inode_sector, 1);
  if(had_add && !success)
    dir_remove(dir, name);

  free(name);
  dir_close(dir);
  lock_release(&temporary);
  return success;
}

/* 在母目录PARENT下创建一个新的目录 ALLOCATED可选，如果ALLOCATED > 0
  那么新目录的inode就在ALLOCATED上   （要将传递进的PARENT关闭）*/
static bool filesys_dir_create(struct inode* parent, block_sector_t allocated, const char* name){
  bool success = false;
  struct dir* dir = NULL;
  struct dir* parent_dir = NULL;
  block_sector_t parent_sector = parent?parent->sector:allocated;

  /* 先新建一个目录 */
  block_sector_t inode_sector = allocated;
  if(allocated == 0)
    if(!free_map_allocate(1, &inode_sector))
      goto done;
  if(!dir_create(inode_sector, 2))
    goto done;
  
  /* 在新目录中添加. 和 ..项*/
  dir = dir_open(inode_open(inode_sector));
  if(dir == NULL || !dir_add(dir, ".", inode_sector) || !dir_add(dir, "..", parent_sector))
    goto done;
  
  if(allocated > 0){
    success = true;
    goto done;
  }
  /* 在旧目录中添加*/
  if((parent_dir = dir_open(parent)) == NULL) 
    goto done;
  
  if(!dir_add(parent_dir, name, inode_sector))
    goto done;
  success = true;
done:
  dir_close(dir);
  dir_close(parent_dir);
  /* 如果创建失败，并且扇区不是由alloc提供，那么就要释放扇区 */
  if(inode_sector > 0 && allocated == 0 && !success)
    free_map_release(inode_sector, 1);

  return success;
}

/* 打开文件 */
struct file* filesys_open(struct dir* cur_dir, const char* path) {
  lock_acquire(&temporary);

  struct inode* inode;
  char *name = NULL;
  struct dir* dir = NULL;
  struct file* file = NULL;

  if((name = filesys_lookup(dir_reopen(cur_dir), path, &inode)) == NULL)
    goto done;

  dir = dir_open(inode);
  if(!dir_lookup(dir, name, &inode))
    goto done;
  file = file_open(inode);

done:

  dir_close(dir);
  free(name);
  lock_release(&temporary);
  return file;
}

/* 删除文件 */
bool filesys_remove(struct dir* cur_dir, const char* path){
  lock_acquire(&temporary);

  struct inode* inode;
  bool success = false;
  char *name = NULL;
  struct dir* dir = NULL;

  if((name = filesys_lookup(dir_reopen(cur_dir), path, &inode)) == NULL)
    goto done;

  dir = dir_open(inode);
  success = dir_remove(dir, name);

done:

  dir_close(dir);
  free(name);
  lock_release(&temporary);
  return success;
}


/* 在CUR_DIR文件夹的基础上，通过PATH路径找到相应文件的上级目录，并且返回文件名字
 ！函数返回时一定要关闭dir ！  因为调用该函数时参数CUR_DIR是重新打开的
  返回的*inode必须是打开状态      */
static char* filesys_lookup(struct dir* cur_dir, const char *path, struct inode** inode){

  bool success = false;
  struct dir* dir = NULL;
  int count = 0;
  char** components;
  bool root;

  /* 解析字符串 */
  if((components = parse_path(path, &count)) == NULL)
    goto done;

  /* 判断使用路径 */
  root = cur_dir == NULL || path[0] == '/' ;
  if(root)
    dir = dir_open_root();
  else dir = cur_dir;
  *inode = dir->inode;

  /* 如果传递的cur_dir不为空，且被删除不执行任何操作 */
  if(cur_dir != NULL && cur_dir->inode->removed)
    goto done;

  /* 打开目录到最后一个'/'之间 */
  for(int i = 0; i < count - 1; i++){
    if(!dir_lookup(dir, components[i], inode))
      goto done;
    
    dir_close(dir);
    dir = dir_open(*inode);    

    if(!dir_is(*inode))
      goto done;
  }

  
  success = true;
done:

  /* 释放CUR_DIR（没有被利用） */
  if(root && cur_dir)
    dir_close(cur_dir);
  /* 释放在目录中遍历的dir，由于需要返回*inode 不能使用dir_close()*/
  free(dir);
  if(!success){
    inode_close(*inode);
    *inode = NULL;
  }
  /* 返回最后一个文件名 */
  char* name = NULL;
  if(success){
    if(count - 1 < 0){
      name = malloc(2);
      if(name){
        name[0] = '.';  name[1] = '\0'; 
      }
    }
    else{
      int len = strlen(components[count-1]);
      name = malloc(len + 1);
      if(name)
        strlcpy(name, components[count-1], len + 1);
    }
  }
  free_path_components(components, count);
  
  return name;
}



/* 进入目录（直接引用filesys_open） */
bool filesys_cd(struct dir** cur_dir, const char* name){
  struct dir* dir = NULL;
  bool success = false;
  struct file *file = filesys_open(*cur_dir, name);

  lock_acquire(&temporary);

  if(file == NULL) goto done;

  dir = dir_open(inode_reopen(file_get_inode(file)));
  if(dir_is(dir->inode))
    success = true;


done:
  file_close(file);
  if(success){
    dir_close(*cur_dir);
    *cur_dir = dir;
  }else
    dir_close(dir);

  lock_release(&temporary);
  return success;
}


/* 创建目录 */
bool filesys_mkdir(struct dir* cur_dir, const char* path){
  lock_acquire(&temporary);

  struct inode* inode;
  bool success = false;
  char *name = NULL;

  if((name = filesys_lookup(dir_reopen(cur_dir), path, &inode)) == NULL)
    goto done;
  
  /* 该函数要将inode关闭！ */
  success = filesys_dir_create(inode, 0, name);
  

done:

  free(name);
  lock_release(&temporary);
  return success;
}

/* Formats the file system. */
static void do_format(void) {
  printf("Formatting file system...");
  free_map_create();
  if (!filesys_dir_create(NULL, ROOT_DIR_SECTOR, NULL))
    PANIC("root directory creation failed");
  free_map_close();
  printf("done.\n");
}

/* 目录路径字符串解析程序 */
static char** parse_path(const char* path, int* count) {
    if (path == NULL || count == NULL) return NULL;
    
    int len = strlen(path);
    if (len == 0) return NULL;
    
    // 分配字符串数组内存（最多10个分量）
    char** components = calloc(10, sizeof(char*));
    if (components == NULL) return NULL;
    
    // 复制路径以便处理
    char* path_copy = malloc(len + 1);
    if (path_copy == NULL) {
        free(components);
        return NULL;
    }
    strlcpy(path_copy, path, len + 1);
    
    // 单次分割路径
    *count = 0;
    char* saveptr = NULL; 
    char* token = strtok_r(path_copy, "/", &saveptr);
    
    while (token != NULL && *count < 30) {
        components[*count] = malloc(strlen(token) + 1);
        if (components[*count] == NULL) {
            // 内存分配失败，清理已分配的内存
            free_path_components(components, *count);
            free(path_copy);
            return NULL;
        }
        strlcpy(components[*count], token, strlen(token) + 1);
        (*count)++;
        token = strtok_r(NULL, "/", &saveptr);  
    }
    
    free(path_copy);
    return components;
}

/* 释放路径解析结果 */
static void free_path_components(char** components, int count) {
    if (components == NULL) return;
    
    for (int i = 0; i < count; i++) {
        if (components[i] != NULL) {
            free(components[i]);
        }
    }
    free(components);
}

/* 释放锁资源 */
void filesys_release_lock(void){
  if(lock_held_by_current_thread(&temporary))
    lock_release(&temporary);
}
