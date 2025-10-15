#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "lib/kernel/console.h"
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "filesys/inode.h"
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/malloc.h"
#include "threads/vaddr.h"
#include "userprog/process.h"
#include "userprog/pagedir.h"
#include "devices/shutdown.h"
#include "devices/input.h"
#include "string.h"
#ifdef VM
#include "vm/frame.h"
#include "vm/page.h"
#endif

#define EOF -1

static void syscall_handler(struct intr_frame*);
static char* string_check(char *str);
static bool write_read_check(char *str, size_t len);
static void check_out_bound(uint32_t* args, int num);

#ifdef VM
static void lock_buffer(char* buffer, size_t len);  
static void release_buffer(char* buffer, size_t len);
#endif

void syscall_init(void) { intr_register_int(0x30, 3, INTR_ON, syscall_handler, "syscall"); }

/* 这里的args数组最多个有4变量，取决于系统调用类型，详见src/lib/user/syscall.c*/
static void syscall_handler(struct intr_frame* f UNUSED) {
  uint32_t* args = ((uint32_t*)f->esp);
  struct process* pcb = thread_current()->pcb;

  if(is_kernel_vaddr(args + 1))
    process_error_exit();

  if (args[0] == SYS_EXIT) {
    check_out_bound(args,2);

    f->eax = args[1];
    printf("%s: exit(%d)\n", pcb->process_name, args[1]);
    process_exit(args[1]);
  }
  
  else if(args[0] == SYS_HALT){
    shutdown_power_off();
  }
  
  else if(args[0] == SYS_EXEC){
     check_out_bound(args,2);

    char *file_name = string_check((char*)args[1]);
    pid_t pid;
    pid = process_execute(file_name);
    f->eax = pid;  
  }

  else if(args[0] == SYS_WAIT){
    check_out_bound(args,2);
    f->eax = process_wait((pid_t)args[1]);
  }
  
  else if(args[0] == SYS_CREATE){
     check_out_bound(args,3);

    char* path = string_check((char*)args[1]);
    f->eax = filesys_create(pcb->dir, path, (off_t)args[2]);
  }

  else if(args[0] == SYS_REMOVE){
     check_out_bound(args,2);

    char* path = string_check((char*)args[1]);
  
    f->eax = filesys_remove(pcb->dir, path);
  }

  else if(args[0] == SYS_OPEN){
    check_out_bound(args,2);

    char* path = string_check((char*)args[1]);
    f->eax = file_get_fd(process_open_file(path));
  }

  else if(args[0] == SYS_FILESIZE){
    check_out_bound(args,2);

    int fd = (int)args[1];
    if(fd >= 0 && fd <= 10){
      struct file *file = pcb->fd_tb[fd];
      if(file == NULL) 
        f->eax = -1;
      else
        f->eax = file_length(file);
    }else
      f->eax = -1;
  }

  else if(args[0] == SYS_READ){
    check_out_bound(args,4);

    int fd = (int)args[1];
    off_t len = (off_t)args[3];
    char* buffer = (char*)args[2];
    bool flag = write_read_check(buffer, len) && (fd >= 0 && fd < 10);

#ifdef VM
    lock_buffer(buffer, len);
#endif 

    if(fd == STDIN_FILENO && flag){
      off_t i;
      for(i = 0; i < len; i++){
        char c = input_getc();
        if(c == '\n'){ f->eax = i; break;}
        if(c == EOF){f->eax = 0; break;}
        buffer[i] = c;
      }
      if(i == len) f->eax = len;
    }else if (fd > 1 && flag){
      /* 这里没有检查权限！ */
      struct file *file = pcb->fd_tb[fd];
      if(file != NULL){
        f->eax = file_read(file, buffer, len);   
      }
      else
        f->eax = -1;
      
    }else
      f->eax = -1;

#ifdef VM
    release_buffer(buffer, len);
#endif 
  }

  else if(args[0] == SYS_WRITE){ 
    check_out_bound(args,4);

    char *buffer = (char*)args[2];
    int fd = (int)args[1];
    off_t len = (off_t)args[3];
    bool flag = write_read_check(buffer, len) && (fd >= 0 && fd < 10);

#ifdef VM
    lock_buffer(buffer, len);
#endif 

    if(fd == STDOUT_FILENO && flag){
      putbuf(buffer, len);               /* 偷懒没分割，可能页没必要分割 */
    }else if(fd > 1 && flag){
      /* 这里没有检查权限！ */
      struct file *file = pcb->fd_tb[fd];
      if(file != NULL){
        f->eax = file_write(file, buffer, len);   
      }
      else
        f->eax = -1;

    }else
      f->eax = -1;

#ifdef VM
    release_buffer(buffer, len);
#endif 
  }

  else if(args[0] == SYS_SEEK){
    check_out_bound(args,3);

    int fd = (int)args[1];
    off_t pos = (off_t)args[2];

    if(fd >=0 && fd < 10){
      struct file *file = pcb->fd_tb[fd];
      if(file != NULL){
        file_seek(file, pos);                
      }   
    } 
  }

  else if(args[0] == SYS_TELL){
    check_out_bound(args,2);

    int fd = (int)args[1];
    if(fd >= 0 && fd < 10){
      struct file *file = pcb->fd_tb[fd];
      if(file != NULL)
        f->eax = file_tell(file);
      else 
        f->eax = -1;
    }else 
      f->eax = -1;

  }

  else if(args[0] == SYS_CLOSE){
    check_out_bound(args,2);

    int fd = (int)args[1];
    if(fd >= 0 && fd < 10){
      struct file* file = pcb->fd_tb[fd];
      if(file != NULL){
        file_close(file);
        pcb->fd_tb[fd] = NULL;
      }
    }
  }

#ifdef VM
  else if(args[0] == SYS_MMAP){
    check_out_bound(args,3);

    int fd = (int)args[1];
    void* uaddr = (void*)args[2];

    if(uaddr > (void*)0x8408000 && uaddr < (void*)PHYS_BASE && !pg_ofs(uaddr)){
      if(fd > 1){
        struct file* file = file_reopen(pcb->fd_tb[fd]);
        if(!mmap_init(pcb, file, uaddr))
          f->eax = -1;
        else
          f->eax = mmap_alloc(pcb, file, uaddr);
      }else
        f->eax = -1;
    }else
      f->eax = -1;

  }

  else if(args[0] == SYS_MUNMAP){
    check_out_bound(args,2);

    int mt = (int)args[1];
    mmap_close(pcb, mt);
  }

#endif
  else if(args[0] == SYS_CHDIR){
    check_out_bound(args,2);

    char* path = string_check((char*)args[1]);
    f->eax = filesys_cd(&pcb->dir, path);
  }

  else if(args[0] == SYS_MKDIR){
    check_out_bound(args,2);

    char* path = string_check((char*)args[1]);
    f->eax = filesys_mkdir(pcb->dir, path);
  }

  else if(args[0] == SYS_READDIR){
    check_out_bound(args,3);

    f->eax = -1;
    char *buffer = (char*)args[2];
    int fd = (int)args[1];
    /* 检查缓冲区和fd */
    bool flag = write_read_check(buffer, READDIR_MAX_LEN) && (fd >= 2 && fd < 10);
    if(!flag) 
      return;
    /* 检查文件是否存在已经是否为目录 */
    struct file* file = pcb->fd_tb[fd];
    struct dir* dir;
    flag = (file != NULL && (dir = dir_open_file(file)) != NULL ) ;
    if(!flag)
      return;
    /* 执行readdir */
    f->eax = dir_readdir(dir, buffer);
    dir_close_file(dir, file);
  }

  else if(args[0] == SYS_ISDIR){
    check_out_bound(args,2);

    f->eax = -1;
    struct file* file;
    int fd = (int)args[1];
    bool flag = (fd >= 2 && fd < 10);
    if(flag){
      file = pcb->fd_tb[fd];
      if(file){
        f->eax = dir_is(file_get_inode(file));
      }
    }
  }

  else if(args[0] == SYS_INUMBER){
    check_out_bound(args,2);

    f->eax = -1;
    struct file* file;
    int fd = (int)args[1];
    bool flag = (fd >= 2 && fd < 10);
    if(flag){
      file = pcb->fd_tb[fd];
      if(file){
        f->eax = (file_get_inode(file)->sector);
      }
    }
  }
}


/* 检查字符串指针，并且复制一份 */
static char* string_check(char *str){ 
  if(str == NULL){
    process_error_exit();
  }

  uintptr_t start_prt = (uintptr_t)str;
  uintptr_t end_prt = (uintptr_t)PHYS_BASE;
  if(start_prt >= end_prt) return NULL;

  size_t max_len = (end_prt - start_prt) < 513 ? (end_prt - start_prt) : 513;
  size_t len = 0;
  while(len < max_len){
    if(str[len] == '\0') break;
    len++;
  }
  if(len >= max_len) return NULL;

  char* s = (char*)malloc(len + 1);
  if(s == NULL) return NULL;
  memcpy(s, str, len);
  s[len] = '\0';
  return s;
}


/*  验证缓冲区是否在安全范围内 */
static bool write_read_check(char *str, size_t len){
  uintptr_t start_prt = (uintptr_t)str;
  uintptr_t end_prt = start_prt + len;

  if(is_user_vaddr((void *)start_prt) && is_user_vaddr((void *)end_prt)){

    return true;
  }
  else
    process_error_exit();
  return false; 
}

/* 栈是否越界 */
static void check_out_bound(uint32_t* args, int num){
  if(is_kernel_vaddr(args + num))
    process_error_exit();
}



#ifdef VM
/*  缓冲区上锁防止被置换 
   有可能缓冲区赖加载，加载到内存，如果访问的地址不存在就直接结束进程 */
static void lock_buffer(char* buffer, size_t len){
  struct process* pcb = thread_current()->pcb;
  void* buffer_start = pg_round_down(buffer);
  void* buffer_end = pg_round_down(buffer + len);

  while(buffer_start <= buffer_end){
    char a = *(char*)buffer_start;     
    void* kaddr = pagedir_get_page(pcb->pagedir, buffer_start);
    frame_set_stable(kaddr, true);
    buffer_start += PGSIZE;
  }
  
}

/* 释放缓冲区的置换锁，允许被加载到交换区中 */
static void release_buffer(char* buffer, size_t len){
  struct process* pcb = thread_current()->pcb;
  void* buffer_start = pg_round_down(buffer);
  void* buffer_end = pg_round_down(buffer + len);

  while(buffer_start <= buffer_end){          
    void* kaddr = pagedir_get_page(pcb->pagedir, buffer_start);
    ASSERT(frame_is_stable(kaddr));
    frame_set_stable(kaddr, false);
    buffer_start += PGSIZE;
  }
}

#endif