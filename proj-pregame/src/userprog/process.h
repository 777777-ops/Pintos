#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/thread.h"
#include <stdint.h>
#include "filesys/directory.h"
#ifdef VM
#include "vm/page.h"
#include "vm/mmap.h"
#endif



// At most 8MB can be allocated to the stack
// These defines will be used in Project 2: Multithreading
#define MAX_STACK_PAGES (1 << 11)
#define MAX_THREADS 127
#define MAX_OPEN_FILE 10

/* PIDs and TIDs are the same type. PID should be
   the TID of the main thread of the process */
typedef tid_t pid_t;

/* Thread functions (Project 2: Multithreading) */
typedef void (*pthread_fun)(void*);
typedef void (*stub_fun)(pthread_fun, void*);



struct process {
  /* Owned by process.c. */
  uint32_t* pagedir;          /* Page directory. */
  char process_name[16];      /* Name of the main thread */
  struct thread* main_thread; /* Pointer to main thread */

  /* 等待机制 */
  struct semaphore sema;        /* 该进程等待结束的锁*/
  pid_t waiting;                /* 该进程等待的进程*/

  /* 继承 */
  struct list children;
  pid_t parent;  

  /* 进程打开的文件 */
  struct file *fd_tb[MAX_OPEN_FILE];  /* 这里2一定是可执行文件 */

  /* 进程当前的工作目录 */
  struct dir* dir;

#ifdef VM
   /* 进程的辅助页表 */
   struct ptable pt;

   /* 文件映射mmap */
   struct mmap* mmap_list;
   size_t mmap_len;
#endif
};

/* 保存子线程的信息 */
struct child_info{
   pid_t child;
   int32_t status;
   bool dead;
   struct list_elem elem;
};




bool child_add(struct process* p, struct process* c);
struct file* process_open_file(const char* name);
int file_get_fd(struct file*);

void userprog_init(void);

bool process_init(struct thread*);
pid_t process_execute(const char* file_name);
int process_wait(pid_t);
void process_exit(int32_t);
void process_activate(void);
void process_error_exit(void);

bool is_main_thread(struct thread*, struct process*);
pid_t get_pid(struct process*);

tid_t pthread_execute(stub_fun, pthread_fun, void*);
tid_t pthread_join(tid_t);
void pthread_exit(void);
void pthread_exit_main(void);

#endif /* userprog/process.h */
