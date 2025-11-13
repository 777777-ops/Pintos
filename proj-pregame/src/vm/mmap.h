#ifndef VM_MMAP_H
#define VM_MMAP_H

#include "stdint.h"
#include "stdio.h"

/* 与辅助页page一样，内存文件映射mmap同样是是由进程维护的 */

/* mmap */
struct mmap{
    struct file* file;
    void* uaddr;
};

size_t mmap_alloc(struct process* pcb, struct file* file, void* uaddr);
bool mmap_init(struct process* pcb, struct file* file, void* uaddr);
void mmap_close(struct process* pcb, int m_t);
#endif