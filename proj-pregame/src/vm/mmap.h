#ifndef VM_MMAP_H
#define VM_MMAP_H

#include "stdint.h"
#include "lib/kernel/list.h"
#include "filesys/file.h"

struct mmap
{
    struct file* file;
    void* uaddr;
    struct list_elem elem;
};




#endif 