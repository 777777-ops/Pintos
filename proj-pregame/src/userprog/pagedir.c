#include "userprog/pagedir.h"
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include "threads/init.h"
#include "threads/pte.h"
#include "threads/palloc.h"
#ifdef VM
#include "vm/frame.h"
#include "vm/page.h"
#include "vm/swap.h"
#endif

static void invalidate_pagedir(uint32_t*);

/* Creates a new page directory that has mappings for kernel
   virtual addresses, but none for user virtual addresses.
   Returns the new page directory, or a null pointer if memory
   allocation fails. */
uint32_t* pagedir_create(void) {
  uint32_t* pd = palloc_get_page(0);
  if (pd != NULL)
    memcpy(pd, init_page_dir, PGSIZE);
  return pd;
}

/* Destroys page directory PD, freeing all the pages it
   references. */
void pagedir_destroy(uint32_t* pd) {
  uint32_t* pde;

  if (pd == NULL)
    return;

  ASSERT(pd != init_page_dir);
  for (pde = pd; pde < pd + pd_no(PHYS_BASE); pde++)
    if (*pde & PTE_P) {
      uint32_t* pt = pde_get_pt(*pde);
      uint32_t* pte;

      for (pte = pt; pte < pt + PGSIZE / sizeof *pte; pte++)
#ifdef VM
        if (*pte & PTE_P){
          struct process* pcb = thread_current()->pcb;
          void *upage = (void*)(((pde - pd) << 22) | ((pte - pt) << 12));
          struct page* page = pages_get(&pcb->pt, (*pte & PTE_AVL) >> 9, upage);
          if(page->type == MMAP && *pte & PTE_D){
            file_seek(page->file, page->pos);
            file_write(page->file, pte_get_page(*pte), page->read_bytes);
          }
          palloc_free_page(pte_get_page(*pte));
        }
        /* 该进程有些页可能被交换到交换分区中，得清理 */
        else if(*pte & PTE_LAZY){
          struct process* pcb = thread_current()->pcb;
          struct page* page = pages_get(&pcb->pt, (*pte & PTE_AVL) >> 9, (void*)(((pde - pd) << 22) | ((pte - pt) << 12)));
          if(page->type == SWAP)
            swap_clean(page->pos);

        }
#else
        if (*pte & PTE_P)
          palloc_free_page(pte_get_page(*pte));
#endif
      palloc_free_page(pt);
    }
  palloc_free_page(pd);
}

/* Returns the address of the page table entry for virtual
   address VADDR in page directory PD.
   If PD does not have a page table for VADDR, behavior depends
   on CREATE.  If CREATE is true, then a new page table is
   created and a pointer into it is returned.  Otherwise, a null
   pointer is returned. */
static uint32_t* lookup_page(uint32_t* pd, const void* vaddr, bool create) {
  uint32_t *pt, *pde;

  ASSERT(pd != NULL);

  /* Shouldn't create new kernel virtual mappings. */
  ASSERT(!create || is_user_vaddr(vaddr));

  /* Check for a page table for VADDR.
     If one is missing, create one if requested. */
  pde = pd + pd_no(vaddr);
  if (*pde == 0) {
    if (create) {
      pt = palloc_get_page(PAL_ZERO);
      if (pt == NULL)
        return NULL;

      *pde = pde_create(pt);
    } else
      return NULL;
  }


  /* Return the page table entry. */
  pt = pde_get_pt(*pde);
  return &pt[pt_no(vaddr)];
}

/* Adds a mapping in page directory PD from user virtual page
   UPAGE to the physical frame identified by kernel virtual
   address KPAGE.
   UPAGE must not already be mapped.
   KPAGE should probably be a page obtained from the user pool
   with palloc_get_page().
   If WRITABLE is true, the new page is read/write;
   otherwise it is read-only.
   Returns true if successful, false if memory allocation
   failed. */
bool pagedir_set_page(uint32_t* pd, void* upage, void* kpage, bool writable) {
  uint32_t* pte;

  ASSERT(pg_ofs(upage) == 0);
  ASSERT(pg_ofs(kpage) == 0);
  ASSERT(is_user_vaddr(upage));
  ASSERT(vtop(kpage) >> PTSHIFT < init_ram_pages);
  ASSERT(pd != init_page_dir);

  pte = lookup_page(pd, upage, true);

  if (pte != NULL) {
    ASSERT((*pte & PTE_P) == 0);
    *pte = pte_create_user(kpage, writable);
#ifdef VM
    frame_set_pcb(kpage, thread_current()->pcb, upage);
#endif
    return true;
  } else
    return false;
}

/* Looks up the physical address that corresponds to user virtual
   address UADDR in PD.  Returns the kernel virtual address
   corresponding to that physical address, or a null pointer if
   UADDR is unmapped. */
void* pagedir_get_page(uint32_t* pd, const void* uaddr) {
  uint32_t* pte;

  ASSERT(is_user_vaddr(uaddr));

  pte = lookup_page(pd, uaddr, false);
  if (pte != NULL && (*pte & PTE_P) != 0)
    return pte_get_page(*pte) + pg_ofs(uaddr);
  else
    return NULL;
}

/* Marks user virtual page UPAGE "not present" in page
   directory PD.  Later accesses to the page will fault.  Other
   bits in the page table entry are preserved.
   UPAGE need not be mapped. */
void pagedir_clear_page(uint32_t* pd, void* upage) {
  uint32_t* pte;

  ASSERT(pg_ofs(upage) == 0);
  ASSERT(is_user_vaddr(upage));

  pte = lookup_page(pd, upage, false);
  if (pte != NULL && (*pte & PTE_P) != 0) {
    *pte &= ~PTE_P;
    invalidate_pagedir(pd);
  }
}

/* 查看该处地址是否有被某页占用(LAZY也算) */
bool pagedir_had_page(uint32_t* pd, void* upage) {
  uint32_t* pte;

  ASSERT(pg_ofs(upage) == 0);
  ASSERT(is_user_vaddr(upage));

  pte = lookup_page(pd, upage, false);
  if (pte != NULL)
    return *pte & PTE_LAZY || *pte & PTE_P;
  return false;
}

/* Returns true if the PTE for virtual page VPAGE in PD is dirty,
   that is, if the page has been modified since the PTE was
   installed.
   Returns false if PD contains no PTE for VPAGE. */
bool pagedir_is_dirty(uint32_t* pd, const void* vpage) {
  uint32_t* pte = lookup_page(pd, vpage, false);
  return pte != NULL && (*pte & PTE_D) != 0;
}

/* Set the dirty bit to DIRTY in the PTE for virtual page VPAGE
   in PD. */
void pagedir_set_dirty(uint32_t* pd, const void* vpage, bool dirty) {
  uint32_t* pte = lookup_page(pd, vpage, false);
  if (pte != NULL) {
    if (dirty)
      *pte |= PTE_D;
    else {
      *pte &= ~(uint32_t)PTE_D;
      invalidate_pagedir(pd);
    }
  }
}

/* Returns true if the PTE for virtual page VPAGE in PD has been
   accessed recently, that is, between the time the PTE was
   installed and the last time it was cleared.  Returns false if
   PD contains no PTE for VPAGE. */
bool pagedir_is_accessed(uint32_t* pd, const void* vpage) {
  uint32_t* pte = lookup_page(pd, vpage, false);
  return pte != NULL && (*pte & PTE_A) != 0;
}

/* Sets the accessed bit to ACCESSED in the PTE for virtual page
   VPAGE in PD. */
void pagedir_set_accessed(uint32_t* pd, const void* vpage, bool accessed) {
  uint32_t* pte = lookup_page(pd, vpage, false);
  if (pte != NULL) {
    if (accessed)
      *pte |= PTE_A;
    else {
      *pte &= ~(uint32_t)PTE_A;
      invalidate_pagedir(pd);
    }
  }
}

/* 获取空闲的3个比特 */
size_t pagedir_get_avl(uint32_t* pd, const void* vpage){
  uint32_t* pte = lookup_page(pd, vpage, false);
  uint32_t avl = *pte & PTE_AVL;
  return avl >> 9;
}

/* 设置空闲的3个比特 */
void pagedir_set_avl(uint32_t* pd, const void* vpage, size_t avl) {
  uint32_t* pte = lookup_page(pd, vpage, false);
  if (pte != NULL) {
    /* 清除原有的AVL位（第9-11位），然后设置新的AVL值 */
    *pte = (*pte & ~PTE_AVL) | ((avl & 0x7) << 9);
  }
}

/* 查看页是否懒加载 */
bool pagedir_is_lazy(uint32_t* pd, const void* vpage) {
  uint32_t* pte = lookup_page(pd, vpage, false);
  return pte != NULL && (*pte & PTE_LAZY) != 0;
}

/* 设置页为赖加载 */
void pagedir_set_lazy(uint32_t* pd, const void* vpage, bool lazy) {
  uint32_t* pte = lookup_page(pd, vpage, true);
  ASSERT(!lazy || (*pte & PTE_P) == 0);
  if (pte != NULL) {
    if (lazy)
      *pte |= PTE_LAZY;
    else {
      *pte &= ~(uint32_t)PTE_LAZY;
      invalidate_pagedir(pd);
    }
  }
}

/* Loads page directory PD into the CPU's page directory base
   register. */
void pagedir_activate(uint32_t* pd) {
  if (pd == NULL)
    pd = init_page_dir;

  /* Store the physical address of the page directory into CR3
     aka PDBR (page directory base register).  This activates our
     new page tables immediately.  See [IA32-v2a] "MOV--Move
     to/from Control Registers" and [IA32-v3a] 3.7.5 "Base
     Address of the Page Directory". */
  /* 这里写入cr3的同时，也会刷新TLB */
  asm volatile("movl %0, %%cr3" : : "r"(vtop(pd)) : "memory");
}

/* Returns the currently active page directory. */
uint32_t* active_pd(void) {
  /* Copy CR3, the page directory base register (PDBR), into
     `pd'.
     See [IA32-v2a] "MOV--Move to/from Control Registers" and
     [IA32-v3a] 3.7.5 "Base Address of the Page Directory". */
  uintptr_t pd;
  asm volatile("movl %%cr3, %0" : "=r"(pd));
  return ptov(pd);
}

/* Seom page table changes can cause the CPU's translation
   lookaside buffer (TLB) to become out-of-sync with the page
   table.  When this happens, we have to "invalidate" the TLB by
   re-activating it.

   This function invalidates the TLB if PD is the active page
   directory.  (If PD is not active then its entries are not in
   the TLB, so there is no need to invalidate anything.) */
static void invalidate_pagedir(uint32_t* pd) {
  if (active_pd() == pd) {
    /* Re-activating PD clears the TLB.  See [IA32-v3a] 3.12
         "Translation Lookaside Buffers (TLBs)". */
    pagedir_activate(pd);
  }
}

/* 在地址UPAGE基础上，直到地址所在页未被加载前，一直向下查找到最后一个已加载页 */
void* pagedir_down_loaded(uint32_t* pd, const void* upage) {
  ASSERT(pd != NULL);
  ASSERT(is_user_vaddr(upage));

  void* uaddr = pg_round_down(upage);
  void* last_loaded = NULL;  
  
  /* 从给定地址所在的页开始向下查找 */
  while (uaddr >= 0) {
    uint32_t* pde = &pd[pd_no(uaddr)];
    
    if (*pde & PTE_P) {
      uint32_t* pt = pde_get_pt(*pde);
      uint32_t* pte = &pt[pt_no(uaddr)];
      
      if (*pte & PTE_P) {
        last_loaded = uaddr;
        uaddr = (void*)((uintptr_t)uaddr - PGSIZE);
        continue;
      }
    }

    return last_loaded;
  }
  
  /* 如果遍历到地址0都没有找到不存在的页，返回最后一个找到的已加载页 */
  return last_loaded;
}