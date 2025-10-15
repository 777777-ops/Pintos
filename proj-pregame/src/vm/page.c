#ifndef VM_PAGE_C
#define VM_PAGE_C

#include "stdbool.h"
#include "threads/vaddr.h"
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "filesys/file.h"
#include "userprog/process.h"
#include "userprog/pagedir.h"
#include "vm/page.h"



static void pages_overflow(struct ptable* pt);



/* 申请一块可执行文件(FILE)的辅助页 */
void pages_reg_exec(struct process* pcb, struct file* file, off_t pos, size_t read_bytes
    , bool write, void* uaddr, bool lazy){
    ASSERT(!lazy || !pagedir_had_page(pcb->pagedir, uaddr));

    struct ptable* pt = &pcb->pt;
    pages_overflow(pt);
    struct page* page = &pt->pages[pt->len];

    page->type = FILE;
    page->file = file;
    page->pos = pos;
    page->read_bytes = read_bytes;

    page->flags |= READ;
    if(write)
        page->flags |= WRITE;
    
    page->uaddr = uaddr;

    size_t avl = pt->len;
    pt->len ++;

    if(lazy)
        pagedir_set_lazy(pcb->pagedir, uaddr, lazy);
    pagedir_set_avl(pcb->pagedir, uaddr, avl);
}

/* 申请一块自定义（SWAP或ZERO）的辅助页*/
void pages_reg(struct process* pcb, void* uaddr, enum swap_type type, bool lazy){
    ASSERT(!lazy || !pagedir_had_page(pcb->pagedir, uaddr));

    struct ptable* pt = &pcb->pt;
    pages_overflow(pt);
    struct page* page = &pt->pages[pt->len];

    page->flags = READ | WRITE;
    page->type = type;
    page->pos = -1;
    page->uaddr = uaddr;

    size_t avl = pt->len;
    pt->len ++;

    if(lazy)
        pagedir_set_lazy(pcb->pagedir, uaddr, lazy);
    pagedir_set_avl(pcb->pagedir, uaddr, avl);

}

/* 申请一块可执行文件(FILE)的辅助页 */
void pages_reg_mmap(struct process* pcb, struct file* file, off_t pos, size_t read_bytes
    , void* uaddr, bool lazy){
    ASSERT(!lazy || !pagedir_had_page(pcb->pagedir, uaddr));

    struct ptable* pt = &pcb->pt;
    pages_overflow(pt);
    struct page* page = &pt->pages[pt->len];

    page->type = MMAP;
    page->file = file;
    page->pos = pos;
    page->read_bytes = read_bytes;

    page->flags |= READ;
    page->flags |= WRITE;
    
    page->uaddr = uaddr;

    size_t avl = pt->len;
    pt->len ++;

    if(lazy)
        pagedir_set_lazy(pcb->pagedir, uaddr, lazy);
    pagedir_set_avl(pcb->pagedir, uaddr, avl);
}


/* 通过页表项的空闲位查找辅助页 */
struct page *pages_get(struct ptable* pt, size_t avl, void* uaddr){
    ASSERT(avl < 8);
    size_t len = pt->len;
    size_t i = avl;
    struct page* page = pt->pages;
    while(i < len){
        if(page[i].uaddr == uaddr)
            return &page[i];
        i += 8;
    }
    PANIC(" NO WAY ");
    return NULL;
}


/* 检查当前辅助页数组大小是否超过限制 */
static void pages_overflow(struct ptable* pt){
    if((pt->len + 1 )* sizeof(struct page) > pt->pages_size){
        pt->pages_size += PGSIZE;
        struct page* old = pt->pages;
        struct page* new = realloc(old, pt->pages_size);
        if(new == NULL){
            free(old);
            PANIC(" NO MEM ");
        }
        pt->pages = new;
    }
}




#endif