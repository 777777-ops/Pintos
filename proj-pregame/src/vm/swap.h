#ifndef VM_SWAP_H
#define VM_SWAP_H

#include "stdint.h"

void swap_clean(int32_t slot);
int32_t swap_out(void* kaddr); 
void swap_in(void* kaddr, int32_t slot);

void swap_init(void);

#endif