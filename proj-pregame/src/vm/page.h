#ifndef VM_PAGE_H
#define VM_PAGE_H

#include "stdbool.h"
#include <stdio.h>
#include "filesys/file.h"


enum swap_type{
    SWAP,
    FILE,
    ZERO,
    MMAP
};

enum page_flags{
    WRITE = 0x1,
    READ = 0x2,
};

struct page{
    enum swap_type type;

    struct file *file;
    off_t pos;               /*可以是槽位，页可以是文件偏移量*/
    size_t read_bytes;       /*需要读取的字节数*/

    uint8_t flags;
    void* uaddr;
};

struct ptable{
    struct page* pages;
    size_t len;
    size_t pages_size;
};


void pages_reg_mmap(struct process*, struct file* file, off_t pos, size_t read_bytes, void* uaddr, bool lazy);
void pages_reg_exec(struct process*, struct file* file, off_t pos, size_t read_bytes, bool write, void* uaddr, bool lazy);
void pages_reg(struct process* pcb, void* uaddr, enum swap_type type, bool lazy);

struct page *pages_get(struct ptable *pt, size_t avl, void* uaddr);




#endif