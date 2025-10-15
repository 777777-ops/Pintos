#include "threads/palloc.h"
#include <bitmap.h>
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "threads/loader.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#ifdef VM
#include "vm/frame.h"
#include "vm/page.h"
#include "vm/swap.h"
#endif
/* Page allocator.  Hands out memory in page-size (or
   page-multiple) chunks.  See malloc.h for an allocator that
   hands out smaller chunks.

   System memory is divided into two "pools" called the kernel
   and user pools.  The user pool is for user (virtual) memory
   pages, the kernel pool for everything else.  The idea here is
   that the kernel needs to have memory for its own operations
   even if user processes are swapping like mad.

   By default, half of system RAM is given to the kernel pool and
   half to the user pool.  That should be huge overkill for the
   kernel pool, but that's just fine for demonstration purposes. */

/* A memory pool. */
struct pool {
  struct lock lock;        /* Mutual exclusion. */
  struct bitmap* used_map; /* Bitmap of free pages. */
  uint8_t* base;           /* Base of pool. */
};

/* Two pools: one for kernel data, one for user pages. */
static struct pool kernel_pool, user_pool;

static void init_pool(struct pool*, void* base, size_t page_cnt, const char* name);
static bool page_from_pool(const struct pool*, void* page);

/* Initializes the page allocator.  At most USER_PAGE_LIMIT
   pages are put into the user pool. */
/*
  这里有个细——————为什么使用uint8_t*解释free_start和free_end?
      
  1.明确：不管使用什么类型的指针解释这里ptov的void*指针，free_start的值都是一样的
    !!!!弄清unit8_t*和unit8_t!!!!
  2.如果是unit32_t*解释这个指针会发生什么？
    unit32_t是4字节长度，unit32_t*指针在计算时，实际值是加n*4
    例如unit32_t* a = 0x1000;  a = a + 1;
    最终:   a == 0x1004 
*/
void palloc_init(size_t user_page_limit) {
  /* Free memory starts at 1 MB and runs to the end of RAM. */
  uint8_t* free_start = ptov(1024 * 1024);
  uint8_t* free_end = ptov(init_ram_pages * PGSIZE);
  size_t free_pages = (free_end - free_start) / PGSIZE;
  size_t user_pages = free_pages / 2;
  size_t kernel_pages;
  if (user_pages > user_page_limit)
    user_pages = user_page_limit;
  kernel_pages = free_pages - user_pages;

  /* Give half of memory to kernel, half to user. */
  init_pool(&kernel_pool, free_start, kernel_pages, "kernel pool");
  init_pool(&user_pool, free_start + kernel_pages * PGSIZE, user_pages, "user pool");

#ifdef VM
  frame_table_init(user_pages);
  uint8_t* i = free_start;
  for(; i < kernel_pool.base; i += PGSIZE)
    frame_create(i, true);

  i = free_start + kernel_pages * PGSIZE;
  extern size_t ker_user_line;
  for(; i < user_pool.base; i += PGSIZE){
    frame_create(i, true);
    ker_user_line ++;
  }
#endif
}

/* Obtains and returns a group of PAGE_CNT contiguous free pages.
   If PAL_USER is set, the pages are obtained from the user pool,
   otherwise from the kernel pool.  If PAL_ZERO is set in FLAGS,
   then the pages are filled with zeros.  If too few pages are
   available, returns a null pointer, unless PAL_ASSERT is set in
   FLAGS, in which case the kernel panics. */
/* 根据flags类型（0,1是内核，2是用户）决定是内核还是用户池分配page_cnt
   个页。 */
void* palloc_get_multiple(enum palloc_flags flags, size_t page_cnt) {
  struct pool* pool = flags & PAL_USER ? &user_pool : &kernel_pool;
  void* pages;
  size_t page_idx;

  if (page_cnt == 0)
    return NULL;

  lock_acquire(&pool->lock);
  page_idx = bitmap_scan_and_flip(pool->used_map, 0, page_cnt, false);
  lock_release(&pool->lock);

  /* 下面进行修改 */
#ifdef VM

  bool is_kernel = !(flags & PAL_USER);
  if (page_idx != BITMAP_ERROR){
    pages = pool->base + PGSIZE * page_idx;
    for(size_t i = 0; i < page_cnt; i++)
      frame_create(pages + i * PGSIZE, is_kernel);
  }
  else if(!is_kernel){
    pages = frame_full_get(page_cnt);
    for(size_t i = 0; i < page_cnt; i++)
      frame_create(pages + i * PGSIZE, is_kernel);
  }else
    pages = NULL;
#else

  if (page_idx != BITMAP_ERROR)
    pages = pool->base + PGSIZE * page_idx;
  else
    pages = NULL;

#endif


  if (pages != NULL) {
    if (flags & PAL_ZERO)
      memset(pages, 0, PGSIZE * page_cnt);
  } else {
    if (flags & PAL_ASSERT)
      PANIC("palloc_get: out of pages");
  }

  return pages;
}

/* Obtains a single free page and returns its kernel virtual
   address.
   If PAL_USER is set, the page is obtained from the user pool,
   otherwise from the kernel pool.  If PAL_ZERO is set in FLAGS,
   then the page is filled with zeros.  If no pages are
   available, returns a null pointer, unless PAL_ASSERT is set in
   FLAGS, in which case the kernel panics. */
/* 既可以分配内核内存池，也可以用户内存池 */
void* palloc_get_page(enum palloc_flags flags) { return palloc_get_multiple(flags, 1); }

/* Frees the PAGE_CNT pages starting at PAGES. */
void palloc_free_multiple(void* pages, size_t page_cnt) {
  struct pool* pool;
  size_t page_idx;

  ASSERT(pg_ofs(pages) == 0);
  if (pages == NULL || page_cnt == 0)
    return;

  if (page_from_pool(&kernel_pool, pages))
    pool = &kernel_pool;
  else if (page_from_pool(&user_pool, pages))
    pool = &user_pool;
  else
    NOT_REACHED();

  page_idx = pg_no(pages) - pg_no(pool->base);

#ifndef NDEBUG
  memset(pages, 0xcc, PGSIZE * page_cnt);
#endif

  ASSERT(bitmap_all(pool->used_map, page_idx, page_cnt));
  bitmap_set_multiple(pool->used_map, page_idx, page_cnt, false);

#ifdef VM
  for(size_t i = 0; i < page_cnt; i++)
    frame_clean(pages + i * PGSIZE);
#endif
}

/* Frees the page at PAGE. */
void palloc_free_page(void* page) { palloc_free_multiple(page, 1); }

/* Initializes pool P as starting at START and ending at END,
   naming it NAME for debugging purposes. */
/* 初始化内存池，这里一般初始化内核池和用户池（两者对半开）。分配page_cnt个
   页的大小，考虑到记录页状态的数据结构位图也需要空间，所以可使用空间只有
   page_cnt - bm_pages个页的大小，并且调整该内存池的起始地址base */
static void init_pool(struct pool* p, void* base, size_t page_cnt, const char* name) {
  /* We'll put the pool's used_map at its base.
     Calculate the space needed for the bitmap
     and subtract it from the pool's size. */
  size_t bm_pages = DIV_ROUND_UP(bitmap_buf_size(page_cnt), PGSIZE);
  if (bm_pages > page_cnt)
    PANIC("Not enough memory in %s for bitmap.", name);
  page_cnt -= bm_pages;

  printf("%zu pages available in %s.\n", page_cnt, name);

  /* Initialize the pool. */
  lock_init(&p->lock);
  p->used_map = bitmap_create_in_buf(page_cnt, base, bm_pages * PGSIZE);
  p->base = base + bm_pages * PGSIZE;
}

/* Returns true if PAGE was allocated from POOL,
   false otherwise. */
static bool page_from_pool(const struct pool* pool, void* page) {
  size_t page_no = pg_no(page);
  size_t start_page = pg_no(pool->base);
  size_t end_page = start_page + bitmap_size(pool->used_map);

  return page_no >= start_page && page_no < end_page;
}
