#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/pagedir.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#ifdef VM
#include "vm/frame.h"
#include "vm/mmap.h"
#endif

extern struct list all_list;
static thread_func start_process NO_RETURN;
static thread_func start_pthread NO_RETURN;
static bool load(const char* comd, void (**eip)(void), void** esp);
bool setup_thread(void (**eip)(void), void** esp);

static struct process* process_get_parent(struct process* c);
static struct child_info* process_get_child(struct process* p, pid_t cpid);
static void process_free(struct process* p);

static int fd_tb_offer(void);
static void fd_tb_init(struct process* pcb);
static void process_close_file(struct process* pcb, struct file* file);

/* Initializes user programs in the system by ensuring the main
   thread has a minimal PCB so that it can execute and wait for
   the first user process. Any additions to the PCB should be also
   initialized here if main needs those members */
/* main线程的初始化 */
void userprog_init(void) {
  struct thread* t = thread_current();
  bool success;

  /* Allocate process control block
     It is imoprtant that this is a call to calloc and not malloc,
     so that t->pcb->pagedir is guaranteed to be NULL (the kernel's
     page directory) when t->pcb is assigned, because a timer interrupt
     can come at any time and activate our pagedir */
  t->pcb = calloc(sizeof(struct process), 1);
  success = t->pcb != NULL;

  if(success){
    //t->pcb->pagedir = init_page_dir;
    t->pcb->pagedir = init_page_dir;
    t->pcb->main_thread = thread_current();
    t->pcb->parent = 0;
    t->pcb->waiting = 0;
    strlcpy(t->pcb->process_name, t->name, sizeof t->name);
    sema_init(&t->pcb->sema,0);
    list_init(&t->pcb->children);
  }   

  /* Kill the kernel if we did not succeed */
  ASSERT(success);
}

/* 初始化线程的pcb  这里的T是子进程*/
bool process_init(struct thread *t){
  /* Allocate process control block */
  struct process* new_pcb = calloc(sizeof(struct process), 1);;   //这里的process在内核池
  bool success = new_pcb != NULL;
  struct process *p = thread_current()->pcb;

  /* Initialize process control block */
  if (success) {
    // Ensure that timer_interrupt() -> schedule() -> process_activate()
    // does not try to activate our uninitialized pagedir
    new_pcb->pagedir = NULL;
    t->pcb = new_pcb;
    // Continue initializing the PCB as normal
    t->pcb->main_thread = t;
    strlcpy(t->pcb->process_name, t->name, sizeof t->name);

    list_init(&t->pcb->children);          
    sema_init(&t->pcb->sema, 0);
    t->pcb->parent = get_pid(p);
    t->pcb->waiting = 0;
    fd_tb_init(t->pcb);
    if(p->dir)
      t->pcb->dir = dir_reopen(p->dir);
#ifdef VM
    t->pcb->pt.pages = malloc(PGSIZE);
    t->pcb->pt.pages_size = PGSIZE;
    t->pcb->mmap_list = calloc(sizeof(struct mmap), 10);
    t->pcb->mmap_len = 0;
#endif
    success = child_add(p, t->pcb);
    if(!success){
      free(t->pcb);
      t->pcb = NULL;
    }
  }


  return success;
}

/* 从指令中挖出第一个单词作为file_name */
static char* file_name_c(const char* comd){
  int i = 0;
  char *file_name;
  // 查找第一个空格或字符串结尾
  while(comd[i] != ' ' && comd[i] != '\0') {
      i++;
  }

  file_name = (char*)malloc(i + 1);
  if(file_name == NULL) return NULL;

  memcpy(file_name, comd, i);
  file_name[i] = '\0';
  return file_name;
}
/* Starts a new thread running a user program loaded from
   FILENAME.  The new thread may be scheduled (and may even exit)
   before process_execute() returns.  Returns the new process's
   process id, or TID_ERROR if the thread cannot be created. */
pid_t process_execute(const char* comd) {
  char* fn_copy;
  tid_t tid;
  char* file_name = file_name_c(comd);

  /* Make a copy of COMD.
     Otherwise there's a race between the caller and load(). */
  fn_copy = palloc_get_page(0);
  if (fn_copy == NULL)
    return TID_ERROR;
  strlcpy(fn_copy, comd, PGSIZE);
 
  /* 查看文件是否存在  这一步目前仅是为了过测试 */
  struct file* file = filesys_open(NULL, file_name);
  if(file == NULL)
    return -1;
  free(file);

  /* Create a new thread to execute FILE_NAME. */
  tid = thread_create(file_name, PRI_DEFAULT, start_process, fn_copy);
  if (tid == TID_ERROR)
    palloc_free_page(fn_copy);
  //  ？


  free(file_name);
  return tid;
}

/* A thread function that loads a user process and starts it
   running. */
static void start_process(void* file_name_) {
  char* file_name = (char*)file_name_;
  struct intr_frame if_;
  bool success;


  /* Initialize interrupt frame and load executable. */
  memset(&if_, 0, sizeof if_);
  if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
  if_.cs = SEL_UCSEG;
  if_.eflags = FLAG_IF | FLAG_MBS;
  success = load(file_name, &if_.eip, &if_.esp);            //这里load可执行程序后的代码在用户池


  /* Handle failure with succesful PCB malloc. Must free the PCB */
  if (!success) {
    // Avoid race where PCB is freed before t->pcb is set to NULL
    // If this happens, then an unfortuantely timed timer interrupt
    // can try to activate the pagedir, but it is now freed memory
    palloc_free_page(file_name);
    process_exit(-2);
  }else{
    palloc_free_page(file_name);
    /* Start the user process by simulating a return from an
      interrupt, implemented by intr_exit (in
      threads/intr-stubs.S).  Because intr_exit takes all of its
      arguments on the stack in the form of a `struct intr_frame',
      we just point the stack pointer (%esp) to our stack frame
      and jump to it. */
    asm volatile("movl %0, %%esp; jmp intr_exit" : : "g"(&if_) : "memory");
    NOT_REACHED();
  }

}

/* Waits for process with PID child_pid to die and returns its exit status.
   If it was terminated by the kernel (i.e. killed due to an
   exception), returns -1.  If child_pid is invalid or if it was not a
   child of the calling process, or if process_wait() has already
   been successfully called for the given PID, returns -1
   immediately, without waiting.

   This function will be implemented in problem 2-2.  For now, it
   does nothing. */
int process_wait(pid_t child_pid) {
  struct thread* cur = thread_current();
  struct process* p = cur->pcb;
  struct child_info* ci = process_get_child(p, child_pid);
  if(ci == NULL) return -1;
  

  enum intr_level old_level = intr_disable();
  if(!ci->dead){
    /* 还没dead */
    p->waiting = child_pid;
    sema_down(&p->sema);
    intr_set_level(old_level);
  }
  intr_set_level(old_level);

  p->waiting = 0;
  int status = ci->status;
  list_remove(&ci->elem);
  free(ci);
  return status;
}

/* 错误退出 */
void process_error_exit(){
  printf("%s: exit(%d)\n", thread_current()->pcb->process_name, -1);
  process_exit(-1);
}

/* Free the current process's resources. */
void process_exit(int32_t status) {
  struct thread* cur = thread_current();
  uint32_t* pd;

  /* If this thread does not have a PCB, don't worry */
  if (cur->pcb == NULL) {
    thread_exit();
    NOT_REACHED();
  }


  /* Destroy the current process's page directory and switch back
     to the kernel-only page directory. */
  /* 这里就重置页目录，要确保重置后的操作不会访问用户空间 */
  pd = cur->pcb->pagedir;
  if (pd != NULL) {
    /* Correct ordering here is crucial.  We must set
         cur->pcb->pagedir to NULL before switching page directories,
         so that a timer interrupt can't switch back to the
         process page directory.  We must activate the base page
         directory before destroying the process's page
         directory, or our active page directory will be one
         that's been freed (and cleared). */
    cur->pcb->pagedir = NULL;
    pagedir_activate(NULL);
    pagedir_destroy(pd);
  }

  /* 允许可执行程序被访问 */
  if(cur->pcb->fd_tb[2] != NULL){
    file_allow_write(cur->pcb->fd_tb[2]);
  }
  

  /* 找到父母进程，保存退出信息 */
  struct process *p = process_get_parent(cur->pcb);
  if(p != NULL){
    struct child_info* c = process_get_child(p, get_pid(cur->pcb));
    if(c == NULL)
      PANIC("DEBUG : NO WAY");

    c->dead = true;
    c->status = status;
  }

  /* 释放pcb*/
  pid_t cur_pit = get_pid(cur->pcb); 
  struct process* pcb_to_free = cur->pcb;
  cur->pcb = NULL;
  process_free(pcb_to_free);

  /* 让等待的父母进程运行 */
  if(p->waiting == cur_pit)
    sema_up(&p->sema);

  /* 释放文件锁 */
  filesys_release_lock();

  thread_exit();
}

/* Sets up the CPU for running user code in the current
   thread. This function is called on every context switch. */
void process_activate(void) {
  struct thread* t = thread_current();

  /* Activate thread's page tables. */
  if (t->pcb != NULL && t->pcb->pagedir != NULL)
    pagedir_activate(t->pcb->pagedir);
  else
    pagedir_activate(NULL);

  /* Set thread's kernel stack for use in processing interrupts.
     This does nothing if this is not a user process. */
  tss_update();
}

/* We load ELF binaries.  The following definitions are taken
   from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;

/* For use with ELF types in printf(). */
#define PE32Wx PRIx32 /* Print Elf32_Word in hexadecimal. */
#define PE32Ax PRIx32 /* Print Elf32_Addr in hexadecimal. */
#define PE32Ox PRIx32 /* Print Elf32_Off in hexadecimal. */
#define PE32Hx PRIx16 /* Print Elf32_Half in hexadecimal. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
   This appears at the very beginning of an ELF binary. */
struct Elf32_Ehdr {
  unsigned char e_ident[16];
  Elf32_Half e_type;
  Elf32_Half e_machine;
  Elf32_Word e_version;
  Elf32_Addr e_entry;
  Elf32_Off e_phoff;
  Elf32_Off e_shoff;
  Elf32_Word e_flags;
  Elf32_Half e_ehsize;
  Elf32_Half e_phentsize;
  Elf32_Half e_phnum;
  Elf32_Half e_shentsize;
  Elf32_Half e_shnum;
  Elf32_Half e_shstrndx;
};

/* Program header.  See [ELF1] 2-2 to 2-4.
   There are e_phnum of these, starting at file offset e_phoff
   (see [ELF1] 1-6). */
struct Elf32_Phdr {
  Elf32_Word p_type;
  Elf32_Off p_offset;
  Elf32_Addr p_vaddr;
  Elf32_Addr p_paddr;
  Elf32_Word p_filesz;
  Elf32_Word p_memsz;
  Elf32_Word p_flags;
  Elf32_Word p_align;
};

/* Values for p_type.  See [ELF1] 2-3. */
#define PT_NULL 0           /* Ignore. */
#define PT_LOAD 1           /* Loadable segment. */
#define PT_DYNAMIC 2        /* Dynamic linking info. */
#define PT_INTERP 3         /* Name of dynamic loader. */
#define PT_NOTE 4           /* Auxiliary info. */
#define PT_SHLIB 5          /* Reserved. */
#define PT_PHDR 6           /* Program header table. */
#define PT_STACK 0x6474e551 /* Stack segment. */

/* Flags for p_flags.  See [ELF3] 2-3 and 2-4. */
#define PF_X 1 /* Executable. */
#define PF_W 2 /* Writable. */
#define PF_R 4 /* Readable. */

static bool setup_stack(void** esp, const char* comd, char **file_name);
static bool validate_segment(const struct Elf32_Phdr*, struct file*);
static bool load_segment(struct file* file, off_t ofs, uint8_t* upage, uint32_t read_bytes,
                         uint32_t zero_bytes, bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
   Stores the executable's entry point into *EIP
   and its initial stack pointer into *ESP.
   Returns true if successful, false otherwise. */
bool load(const char* comd, void (**eip)(void), void** esp) {
  struct thread* t = thread_current();
  struct process* pcb = t->pcb;
  struct Elf32_Ehdr ehdr;
  struct file* file = NULL;
  off_t file_ofs;
  bool success = false;
  int i;
  char *file_name;
  
  /* Allocate and activate page directory. */
  pcb->pagedir = pagedir_create();
  if (pcb->pagedir == NULL)
    goto done;
  process_activate();

  /* Set up stack. */
  if (!setup_stack(esp, comd, &file_name))
    goto done;


  /* Open executable file. */
  /*
  struct file* d = filesys_open(file_name);
  if(d != NULL)
    file_close(d);*/
  file = process_open_file(file_name);
  if (file == NULL) {
    printf("load: %s: open failed\n", file_name);
    goto done;
  }

  /* Read and verify executable header. */
  if (file_read(file, &ehdr, sizeof ehdr) != sizeof ehdr ||
      memcmp(ehdr.e_ident, "\177ELF\1\1\1", 7) || ehdr.e_type != 2 || ehdr.e_machine != 3 ||
      ehdr.e_version != 1 || ehdr.e_phentsize != sizeof(struct Elf32_Phdr) || ehdr.e_phnum > 1024) {
    printf("load: %s: error loading executable\n", file_name);
    goto done;
  }

  /* Read program headers. */
  file_ofs = ehdr.e_phoff;
  for (i = 0; i < ehdr.e_phnum; i++) {
    struct Elf32_Phdr phdr;

    if (file_ofs < 0 || file_ofs > file_length(file))
      goto done;
    file_seek(file, file_ofs);

    if (file_read(file, &phdr, sizeof phdr) != sizeof phdr)
      goto done;
    file_ofs += sizeof phdr;
    switch (phdr.p_type) {
      case PT_NULL:
      case PT_NOTE:
      case PT_PHDR:
      case PT_STACK:
      default:
        /* Ignore this segment. */
        break;
      case PT_DYNAMIC:
      case PT_INTERP:
      case PT_SHLIB:
        goto done;
      case PT_LOAD:
        if (validate_segment(&phdr, file)) {
          bool writable = (phdr.p_flags & PF_W) != 0;
          uint32_t file_page = phdr.p_offset & ~PGMASK;  /* phdr.p_offset 这个是段起始地址（内存）*/
          uint32_t mem_page = phdr.p_vaddr & ~PGMASK;    /* phdr.p_vaddr 这个是段起始地址（内存）*/
          uint32_t page_offset = phdr.p_vaddr & PGMASK;
          uint32_t read_bytes, zero_bytes;
          if (phdr.p_filesz > 0) {
            /* Normal segment.
                     Read initial part from disk and zero the rest. */
            read_bytes = page_offset + phdr.p_filesz;
            zero_bytes = (ROUND_UP(page_offset + phdr.p_memsz, PGSIZE) - read_bytes);
          } else {
            /* Entirely zero.
                     Don't read anything from disk. */
            read_bytes = 0;
            zero_bytes = ROUND_UP(page_offset + phdr.p_memsz, PGSIZE);
          }
          if (!load_segment(file, file_page, (void*)mem_page, read_bytes, zero_bytes, writable))
            goto done;

        } else  
          goto done;
        break;
    }
  }


  /* Start address. */
  *eip = (void (*)(void))ehdr.e_entry;

  success = true;

  file_deny_write(file);                             /* 成功的话要关闭可执行文件的写 */
  return success;                                    /* 不关闭文件 */

done:
  /* We arrive here whether the load is successful or not. */
  process_close_file(t->pcb,file);
  return success;
}

/* load() helpers. */
static bool install_page(void* upage, void* kpage, bool writable);

/* Checks whether PHDR describes a valid, loadable segment in
   FILE and returns true if so, false otherwise. */
static bool validate_segment(const struct Elf32_Phdr* phdr, struct file* file) {
  /* p_offset and p_vaddr must have the same page offset. */
  if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
    return false;

  /* p_offset must point within FILE. */
  if (phdr->p_offset > (Elf32_Off)file_length(file))
    return false;

  /* p_memsz must be at least as big as p_filesz. */
  if (phdr->p_memsz < phdr->p_filesz)
    return false;

  /* The segment must not be empty. */
  if (phdr->p_memsz == 0)
    return false;

  /* The virtual memory region must both start and end within the
     user address space range. */
  if (!is_user_vaddr((void*)phdr->p_vaddr))
    return false;
  if (!is_user_vaddr((void*)(phdr->p_vaddr + phdr->p_memsz)))
    return false;

  /* The region cannot "wrap around" across the kernel virtual
     address space. */
  if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
    return false;

  /* Disallow mapping page 0.
     Not only is it a bad idea to map page 0, but if we allowed
     it then user code that passed a null pointer to system calls
     could quite likely panic the kernel by way of null pointer
     assertions in memcpy(), etc. */
  if (phdr->p_vaddr < PGSIZE)
    return false;

  /* It's okay. */
  return true;
}

/* Loads a segment starting at offset OFS in FILE at address
   UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
   memory are initialized, as follows:

        - READ_BYTES bytes at UPAGE must be read from FILE
          starting at offset OFS.

        - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.

   The pages initialized by this function must be writable by the
   user process if WRITABLE is true, read-only otherwise.

   Return true if successful, false if a memory allocation error
   or disk read error occurs. */

/*  关注upage和kpage，这里由磁盘读入的file文件，被加载到了物理内存池，但
   但由于palloc的特性，尽管kpage映射到了这块物理内存池，但是kpage是虚拟内核
   地址用户进程仍然不可以访问，所以在install_page(upage, kpage, writable)中，
   要将upage这个虚拟用户地址页映射到这块物理内存池，使得用户线程可以访问 */
static bool load_segment(struct file* file, off_t ofs, uint8_t* upage, uint32_t read_bytes,
                         uint32_t zero_bytes, bool writable) {
  ASSERT((read_bytes + zero_bytes) % PGSIZE == 0);
  ASSERT(pg_ofs(upage) == 0);
  ASSERT(ofs % PGSIZE == 0);

  file_seek(file, ofs);
  while (read_bytes > 0 || zero_bytes > 0) {
    /* Calculate how to fill this page.
         We will read PAGE_READ_BYTES bytes from FILE
         and zero the final PAGE_ZERO_BYTES bytes. */
    size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
    size_t page_zero_bytes = PGSIZE - page_read_bytes;

#ifdef VM
    struct process *pcb = thread_current()->pcb;
    if(page_read_bytes > 0){
      pages_reg_exec(pcb, file, ofs, page_read_bytes , writable, upage, true);
      ofs += PGSIZE;
    }
    else 
      pages_reg(pcb, upage, ZERO, true);
#else
    /* Get a page of memory. */
    uint8_t* kpage = palloc_get_page(PAL_USER);
    if (kpage == NULL)
      return false;

    /* Load this page. */
    if (file_read(file, kpage, page_read_bytes) != (int)page_read_bytes) {
      palloc_free_page(kpage);
      return false;
    }
    memset(kpage + page_read_bytes, 0, page_zero_bytes);

    /* Add the page to the process's address space. */
    if (!install_page(upage, kpage, writable)) {
      palloc_free_page(kpage);
      return false;
    }
#endif

    /* Advance. */
    read_bytes -= page_read_bytes;
    zero_bytes -= page_zero_bytes;
    upage += PGSIZE;
  }
  return true;
}

/* 解析命令行参数 */
static bool setup_stack_contents(void** esp, const char* comd, char** file_name) {
  uint8_t* stack_top = (uint8_t*)PHYS_BASE;
  uint8_t* stack_bottom = stack_top - PGSIZE;
  
  // 解析命令字符串
  char* argv[100];
  int i = 0;

  char* cmd_copy = malloc(strlen(comd) + 1);
  if (!cmd_copy) return false;
  strlcpy(cmd_copy, comd, strlen(comd) + 1);
  
  // 第一步：计算参数个数
  int argc = 0;
  
  char* temp_token;
  char* temp_save_ptr;
  for (temp_token = strtok_r(cmd_copy, " ", &temp_save_ptr); temp_token != NULL;
       temp_token = strtok_r(NULL, " ", &temp_save_ptr)) {
    argv[argc++] = temp_token;

    int len = strlen(temp_token) + 1; // 包括null终止符
    stack_top -= len;

    if (stack_top < stack_bottom) {
      free(cmd_copy);
      return false;
    }
  }
  
  if (argc == 0) {
    free(cmd_copy);
    return false; // 没有参数，无效命令
  }
  
  /* 保存文件名 */
  char *file_path = malloc(strlen(argv[0]) + 1);
  if (!file_path) {
    free(cmd_copy);
    return false;
  }
  strlcpy(file_path, argv[0], strlen(argv[0]) + 1);
  *file_name = file_path;


  // 第二步：使用同一个cmd_copy进行参数解析和存储
  uint8_t* temp_stack = stack_top;
  for(i = 0; i < argc; i++){
    int len = strlen(argv[i]) + 1;
    
    memcpy(temp_stack, argv[i], len);
    argv[i] = (char*)temp_stack;
    temp_stack += len;
  }
  free(cmd_copy);
  
  // 字对齐（4字节对齐）
  stack_top -= (uint32_t)stack_top % 4;
  
  // 检查栈溢出
  if (stack_top < stack_bottom) {
    if (file_path) free(file_path);
    return false;
  }
  
  // 压入argv[]指针数组（逆序）
  int argv_array_size = (argc + 1) * sizeof(char*);
  char** argv_ptrs = (char**)(stack_top - argv_array_size);
  
  // 检查栈溢出
  if ((uint8_t*)argv_ptrs < stack_bottom) {
    if (file_path) free(file_path);
    return false;
  }
  
  for (i = 0; i < argc; i++) {
    argv_ptrs[i] = argv[i];
  }
  argv_ptrs[argc] = NULL; // 结束标记
  
  // 更新栈顶
  stack_top = (uint8_t*)argv_ptrs;
  
  /* 最后栈16字节对齐的关键 */
  stack_top -= ((uint32_t)(stack_top + 8) % 16);

  // 压入argv指针
  stack_top -= sizeof(char**);
  if (stack_top < stack_bottom) {
    if (file_path) free(file_path);
    return false;
  }
  *(char***)stack_top = argv_ptrs;
  
  // 压入argc
  stack_top -= sizeof(int);
  if (stack_top < stack_bottom) {
    if (file_path) free(file_path);
    return false;
  }
  *(int*)stack_top = argc;

    // 压入返回地址（0表示程序入口）
  stack_top -= sizeof(void*);
  if (stack_top < stack_bottom) {
    if (file_path) free(file_path);
    return false;
  }
  *(void**)stack_top = 0;
  

  // 设置栈指针
  *esp = stack_top;
  
  return true;
}

/* Create a minimal stack by mapping a zeroed page at the top of
   user virtual memory. */
static bool setup_stack(void** esp, const char* comd, char** file_name) {
  uint8_t* kpage;
  bool success = false;
  void* init_stack = ((uint8_t*)PHYS_BASE) - PGSIZE;

  kpage = palloc_get_page(PAL_USER | PAL_ZERO);
  if (kpage != NULL) {
    success = install_page(init_stack, kpage, true);
    if (success){
      success = setup_stack_contents(esp, comd, file_name);
#ifdef VM
      struct process* pcb = thread_current()->pcb;
      pages_reg(pcb, init_stack, SWAP, false);
#endif
    }
  }

  if(!success)
    palloc_free_page(kpage);
  return success;
}

/* Adds a mapping from user virtual address UPAGE to kernel
   virtual address KPAGE to the page table.
   If WRITABLE is true, the user process may modify the page;
   otherwise, it is read-only.
   UPAGE must not already be mapped.
   KPAGE should probably be a page obtained from the user pool
   with palloc_get_page().
   Returns true on success, false if UPAGE is already mapped or
   if memory allocation fails. */
static bool install_page(void* upage, void* kpage, bool writable) {
  struct thread* t = thread_current();

  /* Verify that there's not already a page at that virtual
     address, then map our page there. */
  bool success = pagedir_get_page(t->pcb->pagedir, upage) == NULL &&
          pagedir_set_page(t->pcb->pagedir, upage, kpage, writable);
  
  return success;
}

/* Returns true if t is the main thread of the process p */
bool is_main_thread(struct thread* t, struct process* p) { return p->main_thread == t; }

/* Gets the PID of a process */
pid_t get_pid(struct process* p) { return (pid_t)p->main_thread->tid; }

/* Creates a new stack for the thread and sets up its arguments.
   Stores the thread's entry point into *EIP and its initial stack
   pointer into *ESP. Handles all cleanup if unsuccessful. Returns
   true if successful, false otherwise.

   This function will be implemented in Project 2: Multithreading. For
   now, it does nothing. You may find it necessary to change the
   function signature. */
bool setup_thread(void (**eip)(void) UNUSED, void** esp UNUSED) { return false; }

/* Starts a new thread with a new user stack running SF, which takes
   TF and ARG as arguments on its user stack. This new thread may be
   scheduled (and may even exit) before pthread_execute () returns.
   Returns the new thread's TID or TID_ERROR if the thread cannot
   be created properly.

   This function will be implemented in Project 2: Multithreading and
   should be similar to process_execute (). For now, it does nothing.
   */
tid_t pthread_execute(stub_fun sf UNUSED, pthread_fun tf UNUSED, void* arg UNUSED) { return -1; }

/* A thread function that creates a new user thread and starts it
   running. Responsible for adding itself to the list of threads in
   the PCB.

   This function will be implemented in Project 2: Multithreading and
   should be similar to start_process (). For now, it does nothing. */
static void start_pthread(void* exec_ UNUSED) {}

/* Waits for thread with TID to die, if that thread was spawned
   in the same process and has not been waited on yet. Returns TID on
   success and returns TID_ERROR on failure immediately, without
   waiting.

   This function will be implemented in Project 2: Multithreading. For
   now, it does nothing. */
tid_t pthread_join(tid_t tid UNUSED) { return -1; }

/* Free the current thread's resources. Most resources will
   be freed on thread_exit(), so all we have to do is deallocate the
   thread's userspace stack. Wake any waiters on this thread.

   The main thread should not use this function. See
   pthread_exit_main() below.

   This function will be implemented in Project 2: Multithreading. For
   now, it does nothing. */
void pthread_exit(void) {}

/* Only to be used when the main thread explicitly calls pthread_exit.
   The main thread should wait on all threads in the process to
   terminate properly, before exiting itself. When it exits itself, it
   must terminate the process in addition to all necessary duties in
   pthread_exit.

   This function will be implemented in Project 2: Multithreading. For
   now, it does nothing. */
void pthread_exit_main(void) {}



/* 添加子进程 */
bool child_add(struct process* p, struct process* c){
  struct child_info* ci = (struct child_info*)malloc(sizeof(struct child_info));
  if(ci == NULL)
    return false;
  
  ci->child = get_pid(c);
  ci->dead = false;
  ci->status = 0;
  list_push_front(&p->children,&ci->elem);
  return true;
}

/* 找到父母进程 */
static struct process* process_get_parent(struct process* c){
  pid_t ppid = c->parent;
  struct list_elem *e;
  for(e = list_begin(&all_list); e != list_end(&all_list); e = list_next(e)){
    struct thread *t= list_entry(e, struct thread, allelem);
    struct process *p = t->pcb;
    if(ppid == get_pid(p))
      return p;
  }
  return NULL;
}

/* 找到孩子进程 */
static struct child_info* process_get_child(struct process* p, pid_t cpid){
  struct child_info* c;
  struct list_elem *e;

  for(e = list_begin(&p->children); e != list_end(&p->children); e = list_next(e)){
    c = list_entry(e, struct child_info, elem);
    if(c->child == cpid)
      return c;
  }
  return NULL;
}

/* 清除一个pcb */
static void process_free(struct process* p){
  struct list* list = &(p->children);
  struct list_elem* e = list_begin(list);
  /* 清理所有子进程信息 */
  while(e != list_end(list)){
    struct list_elem *ce = e;
    e = list_next(e);

    struct child_info* ci = list_entry(ce, struct child_info, elem);
    list_remove(ce);
    free(ci);
  }

  /* 关闭所有已打开文件 */
  int len = sizeof(p->fd_tb)/sizeof(p->fd_tb[0]);
  for(int i = 2; i < len; i++){
    if(p->fd_tb[i] != NULL){
      
      file_close(p->fd_tb[i]);
      p->fd_tb[i] = NULL;
    }
  }

  
#ifdef VM
  /* 释放辅助页 和 mmap*/
  free(p->pt.pages);

  for(size_t i = 0; i < p->mmap_len; i++)
    if(&p->mmap_list[i].file)
      file_close(p->mmap_list[i].file);
  free(p->mmap_list);
#endif 

  free(p);
}

/* 申请一个fd */
static int fd_tb_offer(){
  struct process* cur = thread_current()->pcb;
  int len = sizeof(cur->fd_tb)/sizeof(cur->fd_tb[0]);
  for(int i = 2; i < len; i++){
    if(cur->fd_tb[i] == NULL)
      return i;
  }
  PANIC(" DEBUG ");
  //return -1;
}

int file_get_fd(struct file* file){
  if(file == NULL)  return -1;

  struct process* cur = thread_current()->pcb;
  int len = sizeof(cur->fd_tb)/sizeof(cur->fd_tb[0]);
  for(int i = 2; i < len; i++){
    if(cur->fd_tb[i] == file)
      return i;
  }
  return -1;
}

/* 初始化fd_tb */
static void fd_tb_init(struct process* pcb){
  int len = sizeof(pcb->fd_tb)/sizeof(pcb->fd_tb[0]);
  for(int i = 0; i < len; i++){
    pcb->fd_tb[i] = NULL;
  }
}



/* 打开某个文件 */
struct file* process_open_file(const char* name){

  struct process* cur = thread_current()->pcb;
  struct file* file = filesys_open(cur->dir, name);
  cur->fd_tb[fd_tb_offer()] = file;
  return file;
}

/* 关闭文件 */
static void process_close_file(struct process* pcb, struct file* file){
  int len = sizeof(pcb->fd_tb)/sizeof(pcb->fd_tb[0]);
  for(int i = 0; i < len; i++){
    if(pcb->fd_tb[i] == file){
      pcb->fd_tb[i] = NULL;
      file_close(file);
      return;
    }
  }

  PANIC(" DEBUG ");
}
