#ifndef USERPROG_PAGEDIR_H
#define USERPROG_PAGEDIR_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

uint32_t* pagedir_create(void);
void pagedir_destroy(uint32_t* pd);
bool pagedir_set_page(uint32_t* pd, void* upage, void* kpage, bool rw);
void* pagedir_get_page(uint32_t* pd, const void* upage);
void pagedir_clear_page(uint32_t* pd, void* upage);
bool pagedir_had_page(uint32_t* pd, void* upage);
bool pagedir_is_dirty(uint32_t* pd, const void* upage);
void pagedir_set_dirty(uint32_t* pd, const void* upage, bool dirty);
bool pagedir_is_accessed(uint32_t* pd, const void* upage);
void pagedir_set_accessed(uint32_t* pd, const void* upage, bool accessed);
size_t pagedir_get_avl(uint32_t* pd, const void* upage);
void pagedir_set_avl(uint32_t* pd, const void* upage, size_t avl);
bool pagedir_is_lazy(uint32_t* pd, const void* upage);
void pagedir_set_lazy(uint32_t* pd, const void* upage, bool lazy);
void pagedir_activate(uint32_t* pd);

void* pagedir_down_loaded(uint32_t* pd, const void* upage);

uint32_t* active_pd(void);

#endif /* userprog/pagedir.h */
