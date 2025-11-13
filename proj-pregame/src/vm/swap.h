#ifndef VM_SWAP_H
#define VM_SWAP_H

#include "stdint.h"

/* 磁盘的交换分区，物理页帧内存紧张时，将选定的页帧写入磁盘交换分区 */
void swap_clean(int32_t slot);
int32_t swap_out(void* kaddr); 
void swap_in(void* kaddr, int32_t slot);

void swap_init(void);

#endif