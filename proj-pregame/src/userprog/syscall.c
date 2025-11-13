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
#include "vm/mmap.h"
#endif

/*就系统调用的安全性进行说明：
    syscall的主要任务就是为内核态和用户态搭建起数据交流的桥梁，操作系统
  无法保证用户态的传递的数据可靠性，既有可能因为用户态传递的地址配合已经
  写定的内核处理代码导致数据泄露、系统崩溃诸多安全性问题。
    因此，内核作为桥梁的安全可靠性保证的那一端，必须严谨检查由进程传递的
  地址参数。
  本os中对进程传递的地址参数做了以下三层防护：
    1.对于由进程传递的参数个数做check_out_bound检查，不允许其中的某个参
  数的地址超出虚拟用户边界，即args+N >= PYHS_BASE
    2.对于由进程传递的地址进行检查，借助CPU的MMU机制以及page_fault页处理
  机制，对传递的地址进行“假”访问，由硬件设备反馈
    3.对于由进程传递的地址进行解释检查，有些地址在系统调用中被当成数组(int
  、char)的形式传递，内核需对对这些连续地址进行边界检查，对于传递的需引用
  或需写入的连续空间，还必须将其复制到内核堆(malloc)中，理由在下面讨论*/


/*  在实现了页替换规则机制后，用户进程的空间是有可能被写入到交换分区中的，
  下面讨论某块write缓冲区在FILE文件操作时可能出现的“死锁”情况。恰巧该缓
  冲区被置换到了磁盘交换分区中，那么内核由于需要写入文件数据在向file_system
  申请了文件锁后，再回头访问这块需被置换回的write缓冲区，就会发生“死锁”。
    因此，对于write缓冲区，要求复制同样的数据在不会被交换出的内核池中，
  对于read缓冲区，则是在物理页帧层面，临时将该用户池的页帧设置为stable，
  即无法换出的标志
  */

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
      putbuf(buffer, len);              
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

  if(is_user_vaddr((void *)start_prt) && is_user_vaddr((void *)end_prt))
    return true;
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
/*  缓冲区上锁防止被置换 */
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