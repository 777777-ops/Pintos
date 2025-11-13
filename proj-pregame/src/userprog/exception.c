#include "userprog/exception.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/process.h"
#include "userprog/pagedir.h"
#include "filesys/file.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#ifdef VM
#include "vm/swap.h"
#include "vm/page.h"
#endif

#define MAX_EXTEND 4 * PGSIZE       


/* Number of page faults processed. */
static long long page_fault_cnt;

static void kill(struct intr_frame*);
static void page_fault(struct intr_frame*);

#ifdef VM
static void handle_lazy_load(struct process* pcb, void* fault_addr);
static bool stack_extensible(struct process* pcb, void* fault_addr, void* user_esp);
#endif

/* Registers handlers for interrupts that can be caused by user
   programs.

   In a real Unix-like OS, most of these interrupts would be
   passed along to the user process in the form of signals, as
   described in [SV-386] 3-24 and 3-25, but we don't implement
   signals.  Instead, we'll make them simply kill the user
   process.

   Page faults are an exception.  Here they are treated the same
   way as other exceptions, but this will need to change to
   implement virtual memory.

   Refer to [IA32-v3a] section 5.15 "Exception and Interrupt
   Reference" for a description of each of these exceptions. */
void exception_init(void) {
  /* These exceptions can be raised explicitly by a user program,
     e.g. via the INT, INT3, INTO, and BOUND instructions.  Thus,
     we set DPL==3, meaning that user programs are allowed to
     invoke them via these instructions. */
  intr_register_int(3, 3, INTR_ON, kill, "#BP Breakpoint Exception");
  intr_register_int(4, 3, INTR_ON, kill, "#OF Overflow Exception");
  intr_register_int(5, 3, INTR_ON, kill, "#BR BOUND Range Exceeded Exception");

  /* These exceptions have DPL==0, preventing user processes from
     invoking them via the INT instruction.  They can still be
     caused indirectly, e.g. #DE can be caused by dividing by
     0.  */
  intr_register_int(0, 0, INTR_ON, kill, "#DE Divide Error");
  intr_register_int(1, 0, INTR_ON, kill, "#DB Debug Exception");
  intr_register_int(6, 0, INTR_ON, kill, "#UD Invalid Opcode Exception");
  intr_register_int(7, 0, INTR_ON, kill, "#NM Device Not Available Exception");
  intr_register_int(11, 0, INTR_ON, kill, "#NP Segment Not Present");
  intr_register_int(12, 0, INTR_ON, kill, "#SS Stack Fault Exception");
  intr_register_int(13, 0, INTR_ON, kill, "#GP General Protection Exception");
  intr_register_int(16, 0, INTR_ON, kill, "#MF x87 FPU Floating-Point Error");
  intr_register_int(19, 0, INTR_ON, kill, "#XF SIMD Floating-Point Exception");

  /* Most exceptions can be handled with interrupts turned on.
     We need to disable interrupts for page faults because the
     fault address is stored in CR2 and needs to be preserved. */
  intr_register_int(14, 0, INTR_OFF, page_fault, "#PF Page-Fault Exception");
}

/* Prints exception statistics. */
void exception_print_stats(void) { printf("Exception: %lld page faults\n", page_fault_cnt); }

/* Handler for an exception (probably) caused by a user process. */
static void kill(struct intr_frame* f) {
  /* This interrupt is one (probably) caused by a user process.
     For example, the process might have tried to access unmapped
     virtual memory (a page fault).  For now, we simply kill the
     user process.  Later, we'll want to handle page faults in
     the kernel.  Real Unix-like operating systems pass most
     exceptions back to the process via signals, but we don't
     implement them. */

  /* The interrupt frame's code segment value tells us where the
     exception originated. */
  switch (f->cs) {
    case SEL_UCSEG:
      /* User's code segment, so it's a user exception, as we
         expected.  Kill the user process.  */
      printf("%s: dying due to interrupt %#04x (%s).\n", thread_name(), f->vec_no,
             intr_name(f->vec_no));
      intr_dump_frame(f);
      process_exit(-1);
      NOT_REACHED();

    case SEL_KCSEG:
      /* Kernel's code segment, which indicates a kernel bug.
         Kernel code shouldn't throw exceptions.  (Page faults
         may cause kernel exceptions--but they shouldn't arrive
         here.)  Panic the kernel to make the point.  */
      intr_dump_frame(f);
      PANIC("Kernel bug - unexpected interrupt in kernel");

    default:
      /* Some other code segment? Shouldn't happen. Panic the kernel. */
      printf("Interrupt %#04x (%s) in unknown segment %04x\n", f->vec_no, intr_name(f->vec_no),
             f->cs);
      PANIC("Kernel bug - unexpected interrupt in kernel");
  }
}

/* Page fault handler.  This is a skeleton that must be filled in
   to implement virtual memory.  Some solutions to project 2 may
   also require modifying this code.

   At entry, the address that faulted is in CR2 (Control Register
   2) and information about the fault, formatted as described in
   the PF_* macros in exception.h, is in F's error_code member.  The
   example code here shows how to parse that information.  You
   can find more information about both of these in the
   description of "Interrupt 14--Page Fault Exception (#PF)" in
   [IA32-v3a] section 5.15 "Exception and Interrupt Reference". */
/* [笔者]对page_fault进行了重塑，使得page_fault不再只是输出报错信息，
   而是能够更加灵活地处理缺页、错误地址、错误访问权限          */
/* 首先我们要先理清CPU什么时候触发中断向量14的页错误page_fault()：
      CPU在内核态时:a.无效地址;b.权限不足(尝试写入只读页)c.地址属于
   用户空间，但当前未分配物理页。本os直接断言a和b错误为系统错误，即
   内核代码漏洞。
      至于c情况，会先后判定其是否处于按需分配状态(懒加载)、合法栈拓展
   只有在上面两种情况判断失败后，才断言其是无效地址，但处理方法于a、b
   不同，不断言其是内核代码漏洞，而是认为用户程序异常、危险，终止用户
   程序process_error_exit()*/
static void page_fault(struct intr_frame* f) {
  bool not_present; /* True: not-present page, false: writing r/o page. */
  //bool write;       /* True: access was write, false: access was read. */
  bool user;        /* True: access by user, false: access by kernel. */
  void* fault_addr; /* Fault address. */
  void *user_esp;
  struct process* pcb = thread_current()->pcb;
  ASSERT(pcb->pagedir != NULL);     

  /* Obtain faulting address, the virtual address that was
     accessed to cause the fault.  It may point to code or to
     data.  It is not necessarily the address of the instruction
     that caused the fault (that's f->eip).
     See [IA32-v2a] "MOV--Move to/from Control Registers" and
     [IA32-v3a] 5.15 "Interrupt 14--Page Fault Exception
     (#PF)". */
  asm("movl %%cr2, %0" : "=r"(fault_addr));

  /* Turn interrupts back on (they were only off so that we could
     be assured of reading CR2 before it changed). */
  intr_enable();

  /* Count page faults. */
  page_fault_cnt++;

  /* Determine cause. */
  not_present = (f->error_code & PF_P) == 0;
  //write = (f->error_code & PF_W) != 0;
  user = (f->error_code & PF_U) != 0;
  if(user)
   user_esp = f->esp;
  else
   user_esp = thread_current()->user_esp;       /* 内核引起的栈拓展 */

  /* 页面存在 那么一定是访问权限错误*/
  if(!not_present){
      /*报错信息注释
      printf("Page fault at %p: rights violation error %s page in %s context.\n", fault_addr,
            write ? "writing" : "reading",
            user ? "user" : "kernel");


      printf("%s: dying due to interrupt %#04x (%s).\n", thread_name(), f->vec_no,
             intr_name(f->vec_no));
      intr_dump_frame(f);*/
      process_error_exit();
      return;
  }else{
      /* 内核地址不存在  说明代码有BUG */
      if(is_kernel_vaddr(fault_addr))
         PANIC(" DEBUG ");
         
      

#ifdef VM
      /* 懒加载页 */
      if(pagedir_is_lazy(pcb->pagedir, fault_addr)){
         handle_lazy_load(pcb, fault_addr);
         return;
      }

      /* 判断并处理栈拓展 */
      else if(stack_extensible(pcb, fault_addr, user_esp))
         return;
      else{}
      
#endif

      /* 访问到无效地址 */
      process_error_exit();
  }

}

#ifdef VM


/* 处理懒加载的页，先获取一块用户池的页，然后根据不同情况操作 */
static void handle_lazy_load(struct process* pcb, void* fault_addr){
   void *kaddr = palloc_get_page(PAL_USER);
   if(kaddr == NULL)
      PANIC(" DEBUG ");

   void *uaddr = pg_round_down(fault_addr);
   struct page* page = pages_get(&pcb->pt, pagedir_get_avl(pcb->pagedir, fault_addr), uaddr);
   if(page->type == SWAP)
      swap_in(kaddr, page->pos);
   else if(page->type == FILE || page->type == MMAP){
      int read_bytes = page->read_bytes;
      int zero_bytes = PGSIZE - read_bytes;
      file_seek(page->file, page->pos);
      if(file_read(page->file, kaddr, read_bytes) != read_bytes)
         PANIC(" DEBUG ");
      memset(kaddr + read_bytes, 0, zero_bytes);
   }
   else if(page->type == ZERO)
      memset(kaddr, 0, PGSIZE);
   else  
      PANIC(" NO WAY ");

   size_t avl = pagedir_get_avl(pcb->pagedir, uaddr);
   if(pagedir_get_page(pcb->pagedir, uaddr) == NULL &&
          pagedir_set_page(pcb->pagedir, uaddr, kaddr, page->flags & WRITE))
      pagedir_set_avl(pcb->pagedir, uaddr, avl);
   else
      PANIC(" DEBUG ");
}

/*    不超过最大可拓展范围的就是合理的拓展地址，一旦合理拓展就执行下面操作，
   为缺页地址分配物理页以及辅助页，并且在原栈顶到缺页地址中间懒加载 
   
   关于栈拓展的“合理”有很大的可讨论性：
      (uint32_t)stack_up - (uint32_t)fault_addr <= MAX_EXTEND
      (uint32_t)fault_addr >= (uint32_t)user_esp - 32
      这里的栈拓展判定合理性较为“苛刻”，其不仅要求拓展的fault_addr不能超出
   stack_up栈段顶（区别于栈顶）的某个阈值，甚至还要求不能超过user_esp + 32。
      后者其实是向教学测试的妥协，其教学意义在于使系统实现者考虑到pushall
   这条汇编指令。但在实际生产中这定然不是明智的选择，因为也必须考虑到在某函
   数栈中可能出现int a[1000];之类的大数组分配，那么此时还用32这样8常寄存器
   长度作为阈值显然就不太合理的了。
      其实这也是一种安全性和兼容性的考虑，不过笔者还是更倾向于Linux的设计理
   念（中文翻译）————"宁可错误地允许一些异常的栈访问，也不要错误地拒绝合法
   的栈扩展"

   */
static bool stack_extensible(struct process* pcb, void* fault_addr, void* user_esp){
   uint32_t* pd = pcb->pagedir;
   void* init_stack = ((uint8_t*)PHYS_BASE) - PGSIZE;

   void* stack_up = pagedir_down_loaded(pd, init_stack);
   if((uint32_t)stack_up - (uint32_t)fault_addr > MAX_EXTEND)
      return false;
   if((uint32_t)fault_addr < (uint32_t)user_esp - 32)
      return false;
   
   void* uaddr = pg_round_down(fault_addr);
   void* kaddr =  palloc_get_page(PAL_USER);
   if(pagedir_get_page(pd, uaddr) == NULL &&
          pagedir_set_page(pd, uaddr, kaddr, true)){}
   else PANIC(" DEBUG ");

   /* 分配成功后要创建辅助页 */
   pages_reg(pcb, uaddr, SWAP, false);

   /* 将中间的页懒加载 */
   uaddr += PGSIZE;
   while(uaddr < stack_up){
      pages_reg(pcb, uaddr, SWAP, true);
      uaddr += PGSIZE;
   }
   return true;

}
#endif
