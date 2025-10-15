#ifndef VM_FRAME_C
#define VM_FRAME_C

#include <string.h>
#include "lib/round.h"
#include "filesys/file.h"
#include "threads/loader.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include "vm/frame.h"
#include "vm/swap.h"
#include "vm/page.h"

extern uint32_t init_ram_pages;
struct frame* ftable;   /* 页帧表 */
size_t ker_user_line;   /* 内核用户页帧分界线 <就是内核池的 >=就是用户池的 */
struct lock frame_lock;
size_t clock_prt;       /* 时钟指针 */


static bool frame_used(struct frame* frame);
static bool frame_stable(struct frame* frame);
static struct frame* vtoframe(void* kaddr);
static void* frame_algor(size_t page_cnt);
static void frame_out(void* kaddr);
static void clock_run(void);

/* 初始化页帧表 */
void frame_table_init(size_t user_pages){
    lock_init(&frame_lock);

    size_t haved = 1024 * 1024 / PGSIZE;
    ker_user_line = init_ram_pages - user_pages;
    clock_prt = ker_user_line;

    size_t ft_size = DIV_ROUND_UP(init_ram_pages * sizeof(struct frame), PGSIZE);
    ftable = palloc_get_multiple(PAL_ZERO, ft_size);
    size_t i;
    struct frame* frame;
    /* 低于1MB的内存 + 内核池位图 页帧分配 */
    uint32_t len = haved;
    for(i = 0 ; i < len; i++){
        frame = &ftable[i];
        frame->flags = USED | STABLE;   
        frame->vaddr = ptov(i * PGSIZE);
    }
}

/* 设置页帧的置换状态 */
void frame_set_stable(void* kaddr, bool stable){
    lock_acquire(&frame_lock);

    struct frame* frame = vtoframe(kaddr);
    if(stable)
        frame->flags |= STABLE;
    else
        frame->flags &= ~STABLE;

    lock_release(&frame_lock);
}

/* 查询页帧是否能被置换 */
bool frame_is_stable(void* kaddr){
    lock_acquire(&frame_lock);
    
    struct frame* frame = vtoframe(kaddr);
    bool flag = frame->flags & STABLE;

    lock_release(&frame_lock);
    return flag;
}

/* 设置（用户）页帧所在的进程 以及用户虚拟地址*/
void frame_set_pcb(void *kaddr, struct process* pcb, void *uaddr){
    lock_acquire(&frame_lock);

    ASSERT(is_user_vaddr(uaddr));
    struct frame* frame = vtoframe(kaddr);
    ASSERT(!frame_stable(frame));

    frame->pcb = pcb;
    frame->vaddr = uaddr;

    lock_release(&frame_lock);
}

/* 创建一个新的页帧STABLE可以标志是否是内核池页帧 */
void frame_create(void *kaddr, bool stable){
    lock_acquire(&frame_lock);

    struct frame* frame = vtoframe(kaddr);
    ASSERT(frame->flags == 0);

    if(stable){
        frame->flags = STABLE | USED;
        frame->vaddr = kaddr;
    }else 
        frame->flags = USED;

    lock_release(&frame_lock);
}

/* 清理一个页帧 */
void frame_clean(void *kaddr){
    lock_acquire(&frame_lock);

    struct frame* frame = vtoframe(kaddr);
    memset(frame, 0 ,sizeof(struct frame));

    lock_release(&frame_lock);
}

/* 在页帧用户池满的情况下分配（还未实现多页置换） */
void *frame_full_get(size_t page_cnt){ 
    lock_acquire(&frame_lock);

    void* page = frame_algor(page_cnt);

    lock_release(&frame_lock);
    void* kaddr = page;
    for(size_t i = 0; i < page_cnt; i++){
        lock_acquire(&frame_lock);
        frame_out(kaddr + i * PGSIZE);       
        lock_release(&frame_lock);


        frame_clean(kaddr + i * PGSIZE);
    }



    return page;
}


/*  简单的置换算法:这里笃定所有用户池的页都必须要有pcb，
   如果某一页没有pcb，相信该页处于已分配但未建立映射的状态
     只置换一页                 */
static void* frame_algor(size_t page_cnt){
    size_t i = 0;
    size_t MAX_RUN = init_ram_pages - ker_user_line + 1;
    MAX_RUN *= 2;
    struct frame* frame;
    while(i < MAX_RUN){
        /* 确保clock_prt能分配连续的空间 */
        if(page_cnt != 1 && clock_prt + page_cnt >= init_ram_pages){
            clock_run(); i++; continue;
        }
        if(clock_prt >= init_ram_pages){
            clock_prt = ker_user_line; i++; continue;
        }
            
        size_t debug = 0;
        size_t cnt = 0;
        size_t old_prt = clock_prt;
        for(; cnt < page_cnt; cnt++){
            frame = &ftable[clock_prt];
            clock_run();
            i++;
            /* 该页帧是否没有在使用 */
            if(!frame_used(frame)){
                debug++; 
                continue;
            }
            /* 该页帧是否能被置换 */
            if(frame->pcb && !frame_stable(frame)){
                uint32_t* pd = frame->pcb->pagedir;
                if(pagedir_is_accessed(pd, frame->vaddr))
                    pagedir_set_accessed(pd, frame->vaddr, false);
                else 
                    continue;
            }
            break;                
        }
        if(cnt == page_cnt){
            if(debug == page_cnt)
                PANIC(" DEBUG ");
            frame = &ftable[old_prt];
            uint32_t* pd = frame->pcb->pagedir;
            return pagedir_get_page(pd, frame->vaddr);
        }
    }

    PANIC(" Can't Found ");
}

/* 时钟指针转动 */
static void clock_run(void){
    clock_prt ++;
    if(clock_prt >= init_ram_pages)
        clock_prt = ker_user_line;
}


/* 将内核虚拟地址转为FRAME*/
static struct frame* vtoframe(void* kaddr){
    ASSERT(pg_ofs(kaddr) == 0);
    ASSERT(is_kernel_vaddr(kaddr));

    return &ftable[vtop(kaddr) / PGSIZE];
}


/* 处理将要被清理的页帧，或置出到交换区，或不作任何处理 */
static void frame_out(void* kaddr){
    struct frame* frame = vtoframe(kaddr);
    if(!frame_used(frame))
        return;
    ASSERT(frame->pcb != NULL);
    ASSERT(is_user_vaddr(frame->vaddr));

    size_t index = pagedir_get_avl(frame->pcb->pagedir, frame->vaddr);
    struct page* page = pages_get(&frame->pcb->pt, index, frame->vaddr);
    bool dirty = pagedir_is_dirty(frame->pcb->pagedir, frame->vaddr);

    if(page->type == SWAP)
        page->pos = swap_out(kaddr);
    else if(page->type == FILE || page->type == ZERO){
        if(dirty){
            page->type = SWAP;
            page->pos = swap_out(kaddr);
        }
        /* 既不dirty，也不是交换区的，就不处理 */
    }else if(page->type == MMAP){
        if(dirty){
            file_seek(page->file, page->pos);
            file_write(page->file, kaddr, page->read_bytes);
        }
        /* 不脏就不处理 */
    }else
        PANIC(" DEBUG ");

    /* 换出的用户页要打上标志 */
    pagedir_clear_page(frame->pcb->pagedir, frame->vaddr);
    pagedir_set_lazy(frame->pcb->pagedir, frame->vaddr, true);
    //pagedir_set_avl(frame->pcb->pagedir, frame->vaddr, index);
}



static bool frame_used(struct frame* frame){   return frame->flags & USED;  }
static bool frame_stable(struct frame* frame){   return frame->flags & STABLE;  }

#endif