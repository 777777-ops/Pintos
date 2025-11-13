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


/* 下面给出AMD和Inter官方文档中对于32位架构下，页表项、页目录项的结构
31 _____________________ 12 11 ___ 9 8 7 6 5 4 3 2 1 0
   |                     |   |     | | | | | | | | | |
   |   Page Frame Number |   | AVL |0|0|D|A|0|0|U|R|P|
   |       (20 bits)     |   |     | | | | | | | |/| |
   |_____________________|   |_____| | | | | | | |/|_|
        在本微型os中，只关注空余的3位AVL，以及第0位（Present位）和第1位
    (本系统中当某一物理页帧还未具体分配时，第1位是Lazy位，标志该页虽然
    没有对应的物理页帧，但使用Lazy位占位，交付page_fault处理)
    ---------------------
    所以可能会出现两种情况 
        a.第1、0位分别是 ? : 1  代表该物理页帧存在，建立映射，此时第1位
        也就是AMD和Inter官方文档中Write/Read标志位

        b.第1、0位分别是 1 : 0  代表该物理页帧虽然不存在，但虚拟内存中
        的这块页虚拟页已经被占用（懒加载）
    -------------------
        当进程要在虚拟内存中申请一块空间用于某些用途时，都需要执行本文件中实
    现的pages_reg_xxx()函数，以此在辅助页表中添加一个辅助页（page）记录信息。
    page.uaddr用作独特标识记录该虚拟地址，当内核遇到一个uaddr时，可以通过比
    对pcb中的辅助页表中的page.uaddr找到该虚拟页对应的辅助页
        上面的的搜索过程是N个搜索个数，N代表该进程中的辅助页个数，如果利用上空余
    的3位AVL作为类似基准的作用，可以将让搜索个数减低到N/8个，尽管还是线性复杂度，
    但还是尽可能地利用了架构提供的空闲位。
    */


static void pages_overflow(struct ptable* pt);

/* 下面实现的pages_reg_xxx()函数有一点特别说明
        在严格检测漏洞的ASSERT断言阶段，正常情况下，只需要断言!(pagedir_had_page)
    也就是该虚拟页尚未被占位，但这里还允许了被占位、!lazy的情况。这是因为，本os
    虽然鼓励对每一页都进行按需分配的赖加载，但仍有少数特殊情况的页是不能懒加载的，
    需要在申请虚拟内存时直接建立对物理内存的映射。（例如每个进程的第一个stack页）
        对于这种特殊情况，背离了原先预设的  
    pages_reg_xx()申请辅助页 --> page_fault处理 -->实加载物理RAM页帧
        而是
    实加载物理RAM页帧  --> pages_reg_xx()申请辅助页 
    
*/

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

/* 申请一块交换分区(SWAP)或全零页(ZERO)的辅助页*/
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

/* 申请一块MMAP文件映射的辅助页 */
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


/* 检查当前进程（pcb）中的辅助页数组大小是否超过数组长度（进程初始化时分配一页）， 
    如果超过数组长度，要进行扩容 */
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