#ifndef VM_FRAME_H
#define VM_FRAME_H

#include "userprog/process.h"

/*      物理页帧是以RAM物理内存为视角管理整个物理内存，
    其在本os的主要作用是处理内存空间紧张时的页替换 */

/* 页帧标志 */
enum frame_flags{
    USED = 0x01,             /* 是否使用 */
    STABLE = 0x04            /* 是否可以被移出 */
};

/* 页帧单元 */
struct frame{
    //如果是用户帧的话，vaddr是所属进程页的用户虚拟地址（<PHYS_BASE）
    void* vaddr;
    struct process* pcb;
    uint8_t flags;
};

void frame_table_init(size_t user_pages);

void frame_create(void *kaddr, bool stable);
void *frame_full_get(size_t page_cnt);

void frame_clean(void *kaddr);

void frame_set_pcb(void *kaddr, struct process* pcb, void *uaddr);
void frame_set_stable(void* kaddr, bool stable);
bool frame_is_stable(void* kaddr);
#endif