#ifndef VM_SWAP_C
#define VM_SWAP_C

#include "lib/debug.h"
#include "lib/kernel/bitmap.h"
#include "devices/block.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/vaddr.h"
#include "threads/synch.h"
#include "vm/swap.h"

#define PAGE_SECTORS PGSIZE/BLOCK_SECTOR_SIZE        

struct block* swap_device;
static struct bitmap* free_map;    
struct lock swap_lock;

static uint8_t zero_buffer[BLOCK_SECTOR_SIZE] = {0};    /* DEBUG使用的清空缓冲 */

/* 初始化交换分区 */
void swap_init(void){
    swap_device = block_get_role(BLOCK_SWAP);
    if (swap_device == NULL)
        PANIC("No swap device found, can't initialize swap block.");

    int a = PAGE_SECTORS;
    free_map = bitmap_create(block_size(swap_device)/a);
    if (free_map == NULL)
        PANIC("bitmap creation failed--file system device is too large");
    
    lock_init(&swap_lock);
}


/* 清空某一交换分区 */
void swap_clean(int32_t slot){
    ASSERT(slot >= 0);
    size_t swap_idx = (size_t)slot;
    lock_acquire(&swap_lock);

    bitmap_set(free_map, swap_idx, false);
    for(int i = 0; i < PAGE_SECTORS; i++)
        block_write(swap_device, swap_idx * PAGE_SECTORS + i, zero_buffer);

    
    lock_release(&swap_lock);
}

/* 内存中的数据写入交换分区 */
int32_t swap_out(void* kaddr){
    ASSERT(is_kernel_vaddr(kaddr));
    lock_acquire(&swap_lock);

    size_t swap_idx = bitmap_scan_and_flip(free_map, 0, 1, false);
    for(int i = 0; i < PAGE_SECTORS; i++)
        block_write(swap_device, swap_idx * PAGE_SECTORS + i, kaddr + i * BLOCK_SECTOR_SIZE);

    lock_release(&swap_lock);
    return swap_idx;
}

/* 交换分区中的数据写入内存 */
void swap_in(void* kaddr, int32_t slot){
    ASSERT(is_kernel_vaddr(kaddr));
    ASSERT(slot >= 0); 
    size_t swap_idx = (size_t)slot;
    lock_acquire(&swap_lock);

    bitmap_set(free_map, swap_idx, false);
    for(int i = 0; i < PAGE_SECTORS; i++){
        block_read(swap_device, swap_idx * PAGE_SECTORS + i, kaddr + i * BLOCK_SECTOR_SIZE);
        block_write(swap_device, swap_idx * PAGE_SECTORS + i, zero_buffer);
    }

    lock_release(&swap_lock);
}


#endif