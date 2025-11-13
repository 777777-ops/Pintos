#ifndef VM_MMAP_C
#define VM_MMAP_C

#include <string.h>
#include "lib/debug.h"
#include "threads/vaddr.h"
#include "threads/thread.h"
#include "threads/palloc.h"
#include "userprog/process.h"
#include "userprog/pagedir.h"
#include "vm/mmap.h"

/* mmap 
struct mmap{
    struct file* file;
    void* uaddr;
};
*/

/* 在当前进程中分配一块mmap内存映射 */
size_t mmap_alloc(struct process* pcb, struct file* file, void* uaddr){
  ASSERT(pcb->mmap_len < 10);
  ASSERT(!pg_ofs(uaddr));

  size_t len = pcb->mmap_len;
  struct mmap* mmap = &pcb->mmap_list[len];
  mmap->file = file;
  mmap->uaddr = uaddr;
  pcb->mmap_len ++;
  
  return len;
}

/* 在指定的mmap内存建立映射 */
bool mmap_init(struct process* pcb, struct file* file, void* uaddr){
  ASSERT(!pg_ofs(uaddr));
  size_t size = file_length(file);
  void* upage = uaddr;

  /* 检查内存是否被占用 */
  for(size_t i = 0 ; i < size; i+=PGSIZE){
    if(pagedir_had_page(pcb->pagedir, upage))
      return false;
    upage += PGSIZE;
  } 

  upage = uaddr;

  for(size_t i = 0 ; i < size; i+=PGSIZE){
    size_t read_bytes = size - i > PGSIZE ? PGSIZE:(size - i);
    pages_reg_mmap(pcb, file, i, read_bytes, upage, true);

    upage += PGSIZE;
  } 
  return true;
}


/* unmmap */
void mmap_close(struct process* pcb, int m_t){
  ASSERT(m_t >= 0 && (size_t)m_t < pcb->mmap_len);

  struct mmap* mmap = &pcb->mmap_list[m_t];
  if(mmap->file){
    void* upage = mmap->uaddr;
    struct file* file = mmap->file;
    size_t size = file_length(file);


    for(size_t i = 0 ; i < size; i += PGSIZE){
      struct page* page = pages_get(&pcb->pt, pagedir_get_avl(pcb->pagedir, upage), upage);
      void* kpage = pagedir_get_page(pcb->pagedir, upage);        /* kpage != NULL 说明在物理页中*/
      ASSERT(page->type == MMAP);
      if(kpage && pagedir_is_dirty(pcb->pagedir, upage)){
        file_seek(page->file, page->pos);
        file_write(page->file, kpage, page->read_bytes);
        /* 释放页帧 */
        palloc_free_page(kpage);
      }
      pagedir_set_lazy(pcb->pagedir, upage, false);
      pagedir_clear_page(pcb->pagedir, upage);
      upage += PGSIZE;
    } 

    file_close(mmap->file);
    memset(mmap, 0, sizeof(struct mmap));
  }
}
#endif