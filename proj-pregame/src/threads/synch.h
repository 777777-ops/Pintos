#ifndef THREADS_SYNCH_H
#define THREADS_SYNCH_H

#include <list.h>
#include <stdbool.h>


void synch_init(void);

/* A counting semaphore. */
struct semaphore {
  unsigned value;      /* Current value. */
  struct list waiters; /* List of waiting threads. */
};

void sema_init(struct semaphore*, unsigned value);
void sema_down(struct semaphore*);
bool sema_try_down(struct semaphore*);
void sema_up(struct semaphore*);
void sema_self_test(void);

/* Lock. */
struct lock {
  struct thread* holder;      /* Thread holding lock (for debugging). */
  struct semaphore semaphore; /* Binary semaphore controlling access. */
};

void lock_init(struct lock*);
void lock_acquire(struct lock*);
bool lock_try_acquire(struct lock*);
void lock_release(struct lock*);
bool lock_held_by_current_thread(const struct lock*);

/* Condition variable. */
struct condition {
  struct list waiters; /* List of waiting threads. */
};

void cond_init(struct condition*);
void cond_wait(struct condition*, struct lock*);
void cond_signal(struct condition*, struct lock*);
void cond_broadcast(struct condition*, struct lock*);

/* Readers-writers lock. */
#define RW_READER 1
#define RW_WRITER 0

struct rw_lock {
  struct lock lock;
  struct condition read, write;
  int AR, WR, AW, WW;
};

void rw_lock_init(struct rw_lock*);
void rw_lock_acquire(struct rw_lock*, bool reader);
void rw_lock_release(struct rw_lock*, bool reader);

/* 捐赠调度策略使用*/
struct thread_priority{
  struct list_elem elem;
  struct thread* thread;
  int old_priorities;
  struct list locks;    /*该线程锁所接受到的，来自于锁的捐赠*/
  struct lock* waiting;
};

struct lock_offer{
  struct list_elem elem;
  struct lock* lock;
  int priority;
};

void create_thread_unit(struct thread* thread);
struct thread_priority* thread_unit(struct thread* t);
void re_priority_prio(struct thread_priority* t_p);

/* Optimization barrier.

   The compiler will not reorder operations across an
   optimization barrier.  See "Optimization Barriers" in the
   reference guide for more information.*/
#define barrier() asm volatile("" : : : "memory")

#endif /* threads/synch.h */
