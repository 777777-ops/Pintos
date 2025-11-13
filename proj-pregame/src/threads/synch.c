/* This file is derived from source code for the Nachos
   instructional operating system.  The Nachos copyright notice
   is reproduced in full below. */

/* Copyright (c) 1992-1996 The Regents of the University of California.
   All rights reserved.

   Permission to use, copy, modify, and distribute this software
   and its documentation for any purpose, without fee, and
   without written agreement is hereby granted, provided that the
   above copyright notice and the following two paragraphs appear
   in all copies of this software.

   IN NO EVENT SHALL THE UNIVERSITY OF CALIFORNIA BE LIABLE TO
   ANY PARTY FOR DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR
   CONSEQUENTIAL DAMAGES ARISING OUT OF THE USE OF THIS SOFTWARE
   AND ITS DOCUMENTATION, EVEN IF THE UNIVERSITY OF CALIFORNIA
   HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

   THE UNIVERSITY OF CALIFORNIA SPECIFICALLY DISCLAIMS ANY
   WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
   WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
   PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS ON AN "AS IS"
   BASIS, AND THE UNIVERSITY OF CALIFORNIA HAS NO OBLIGATION TO
   PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR
   MODIFICATIONS.
*/

#include "threads/synch.h"
#include <stdio.h>
#include <string.h>
#include "threads/interrupt.h"
#include "threads/malloc.h"
#include "threads/thread.h"
#include "lib/kernel/list.h"

/* */
struct list priorities_list;

/* 初始化 */
void synch_init(){
  if(active_sched_policy == SCHED_PRIO){
    create_thread_unit(thread_current());
  }
}
/* Initializes semaphore SEMA to VALUE.  A semaphore is a
   nonnegative integer along with two atomic operators for
   manipulating it:

   - down or "P": wait for the value to become positive, then
     decrement it.

   - up or "V": increment the value (and wake up one waiting
     thread, if any). */
void sema_init(struct semaphore* sema, unsigned value) {
  ASSERT(sema != NULL);

  sema->value = value;
  list_init(&sema->waiters);
}

/* Down or "P" operation on a semaphore.  Waits for SEMA's value
   to become positive and then atomically decrements it.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but if it sleeps then the next scheduled
   thread will probably turn interrupts back on. */
void sema_down(struct semaphore* sema) {
  enum intr_level old_level;

  ASSERT(sema != NULL);
  ASSERT(!intr_context());

  old_level = intr_disable();
  while (sema->value == 0) {
    list_push_back(&sema->waiters, &thread_current()->elem);
    thread_block();
  }
  sema->value--;
  intr_set_level(old_level);
}

/* Down or "P" operation on a semaphore, but only if the
   semaphore is not already 0.  Returns true if the semaphore is
   decremented, false otherwise.

   This function may be called from an interrupt handler. */
/* 不会阻塞，一般使用于中断处理器 */
bool sema_try_down(struct semaphore* sema) {
  enum intr_level old_level;
  bool success;

  ASSERT(sema != NULL);

  old_level = intr_disable();
  if (sema->value > 0) {
    sema->value--;
    success = true;
  } else
    success = false;
  intr_set_level(old_level);

  return success;
}

/* 弹出等待线程中priority最高的线程 */
static struct thread* waiters_pop(struct semaphore* sema){
  struct list_elem* max = list_max(&sema->waiters, pri_comparator, NULL);
  list_remove(max);
  return list_entry(max, struct thread, elem);
}
/* Up or "V" operation on a semaphore.  Increments SEMA's value
   and wakes up one thread of those waiting for SEMA, if any.

   This function may be called from an interrupt handler. */
void sema_up(struct semaphore* sema) {
  enum intr_level old_level;

  ASSERT(sema != NULL);

  old_level = intr_disable();
  if (!list_empty(&sema->waiters))
    thread_unblock(waiters_pop(sema));
  sema->value++;
  intr_set_level(old_level);

  /* 切换 */
  if(!intr_context())
    thread_try_yield();
}

static void sema_test_helper(void* sema_);

/* Self-test for semaphores that makes control "ping-pong"
   between a pair of threads.  Insert calls to printf() to see
   what's going on. */
void sema_self_test(void) {
  struct semaphore sema[2];
  int i;

  printf("Testing semaphores...");
  sema_init(&sema[0], 0);
  sema_init(&sema[1], 0);
  thread_create("sema-test", PRI_DEFAULT, sema_test_helper, &sema);
  for (i = 0; i < 10; i++) {
    sema_up(&sema[0]);
    sema_down(&sema[1]);
  }
  printf("done.\n");
}

/* Thread function used by sema_self_test(). */
static void sema_test_helper(void* sema_) {
  struct semaphore* sema = sema_;
  int i;

  for (i = 0; i < 10; i++) {
    sema_down(&sema[0]);
    sema_up(&sema[1]);
  }
}


static void lock_acquire_prio(struct lock*);
static bool lock_comparator(const struct list_elem* a, const struct list_elem* b, void* aux UNUSED);

/* 查找一个线程对应的线程优先级单元 */
struct thread_priority* thread_unit(struct thread* t){
  ASSERT(t != NULL);

  if(list_empty(&priorities_list))
    return NULL;

  struct list_elem *e;
  struct thread_priority* t_p;
  for (e = list_begin(&priorities_list); e != list_end(&priorities_list); e = list_next(e))
    if((t_p = list_entry(e, struct thread_priority, elem))->thread == t)
      return t_p;
  
  ASSERT("EORRO");
  return NULL;  
}

/* 更新线程优先级 由于维护的locks链表中，元素根据prio从大到小排序，
  所以直接查看第一个元素的prio，判断是否要更新     */
void re_priority_prio(struct thread_priority* t_p){
  
  int priority;
  struct list_elem* elem;
  if(list_empty(&t_p->locks)){
    priority = t_p->old_priorities;
  }else{
    elem = list_front(&t_p->locks);
    priority = list_entry(elem, struct lock_offer, elem)->priority;
  }

  t_p->thread->priority = priority;
  if(t_p->thread == thread_current())
    thread_try_yield();
}

/* 在线程优先级单元中的locks中寻找lock_offer,没有就创建*/
static struct lock_offer* target(struct lock* lock, struct thread_priority* t_p){

  struct list_elem* e;
  struct list* list = &t_p->locks;
  struct lock_offer* lock_offer;
  for (e = list_begin(list); e != list_end(list); e = list_next(e))
    if((lock_offer = list_entry(e, struct lock_offer, elem))->lock == lock)
      return lock_offer;
  lock_offer = (struct lock_offer*)malloc(sizeof(struct lock_offer));
  lock_offer->lock = lock;
  lock_offer->priority = -1;
  return lock_offer;
}

/* 向低优先级线程捐赠 */
static void offer_lock(struct lock* lock, struct thread* low, int priority){
  ASSERT(lock != NULL);
  ASSERT(low != NULL);

  struct thread_priority* t_p = thread_unit(low);
  ASSERT(t_p != NULL);

  struct lock_offer* offer = target(lock,t_p);
  
  if(offer->priority == -1) {
    offer->priority = priority;
  }else if(offer->priority < priority){
    offer->priority = priority;
    list_remove(&offer->elem);
  }else{}
  
  list_insert_ordered_down(&t_p->locks, &offer->elem, lock_comparator, NULL);
  re_priority_prio(t_p);
}

/* A <= B时 返回true */
static bool lock_comparator(const struct list_elem* a, const struct list_elem* b, void* aux UNUSED){
  return list_entry(a, struct lock_offer, elem)->priority <= list_entry(b, struct lock_offer, elem)->priority;
}

/* 迭代捐赠 */
static void nested_donate(struct thread_priority* cur_tp){
  ASSERT(cur_tp != NULL);

  struct thread_priority* tp = cur_tp;
  struct lock* lock = cur_tp->waiting;
  struct thread* holder;
  while(lock != NULL){
    holder = lock->holder;
    offer_lock(lock, holder, cur_tp->thread->priority);

    tp = thread_unit(holder);
    lock = tp->waiting;
  }
}

/* 结束捐献 */
static void end_offer(struct lock* lock, struct thread_priority* holder){
  ASSERT(lock != NULL);

  struct thread_priority* t_p = holder;
  struct list_elem* e;
  struct list* list = &t_p->locks;
  struct lock_offer* lock_offer;
  for (e = list_begin(list); e != list_end(list); e = list_next(e))
    if((lock_offer = list_entry(e, struct lock_offer, elem))->lock == lock){
      list_remove(e);
      re_priority_prio(t_p);
      return;
    }

  ASSERT("EROOR");
  
}

/* 创建一个线程优先级单元 */
void create_thread_unit(struct thread* thread){
  ASSERT(thread != NULL);

  struct thread_priority* thread_p = (struct thread_priority*)malloc(sizeof(struct thread_priority));
  list_push_front(&priorities_list,&thread_p->elem);
  list_init(&thread_p->locks);
  thread_p->old_priorities = thread->priority;
  thread_p->thread = thread;
  thread_p->waiting = NULL;
}
/* Initializes LOCK.  A lock can be held by at most a single
   thread at any given time.  Our locks are not "recursive", that
   is, it is an error for the thread currently holding a lock to
   try to acquire that lock.

   A lock is a specialization of a semaphore with an initial
   value of 1.  The difference between a lock and such a
   semaphore is twofold.  First, a semaphore can have a value
   greater than 1, but a lock can only be owned by a single
   thread at a time.  Second, a semaphore does not have an owner,
   meaning that one thread can "down" the semaphore and then
   another one "up" it, but with a lock the same thread must both
   acquire and release it.  When these restrictions prove
   onerous, it's a good sign that a semaphore should be used,
   instead of a lock. */
void lock_init(struct lock* lock) {
  ASSERT(lock != NULL);

  lock->holder = NULL;
  sema_init(&lock->semaphore, 1);
}

/* Acquires LOCK, sleeping until it becomes available if
   necessary.  The lock must not already be held by the current
   thread.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but interrupts will be turned back on if
   we need to sleep. */

void lock_acquire(struct lock* lock) {
  ASSERT(lock != NULL);
  ASSERT(!intr_context());
  ASSERT(!lock_held_by_current_thread(lock));

  if(active_sched_policy == SCHED_PRIO){
    lock_acquire_prio(lock);
  }else{
    sema_down(&lock->semaphore);
    lock->holder = thread_current();
  }
}

/* 优先级捐赠策略实现：*/
static void lock_acquire_prio(struct lock* lock){
  
  if(lock_try_acquire(lock)) return;

  struct thread* holder;
  struct thread* cur = thread_current();
  struct thread_priority* cur_tp = thread_unit(cur); 
  enum intr_level old_level;

  old_level = intr_disable();
  holder = lock->holder;
  //什么时候holder会大于cur，如果真的大于又怎么处理
  if(holder!=NULL){  
  
    cur_tp->waiting = lock;
    nested_donate(cur_tp);

    sema_down(&lock->semaphore);
    lock->holder = thread_current();
    cur_tp->waiting = NULL;

  }else{
    sema_down(&lock->semaphore);
    lock->holder = thread_current();
  }
  intr_set_level(old_level);
}

/* Tries to acquires LOCK and returns true if successful or false
   on failure.  The lock must not already be held by the current
   thread.

   This function will not sleep, so it may be called within an
   interrupt handler. */
bool lock_try_acquire(struct lock* lock) {
  bool success;

  ASSERT(lock != NULL);
  ASSERT(!lock_held_by_current_thread(lock));

  success = sema_try_down(&lock->semaphore);
  if (success)
    lock->holder = thread_current();
  return success;
}

static void lock_release_prio(struct lock* lock);

/* Releases LOCK, which must be owned by the current thread.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to release a lock within an interrupt
   handler. */
void lock_release(struct lock* lock) {
  ASSERT(lock != NULL);
  ASSERT(lock_held_by_current_thread(lock));

  if(active_sched_policy == SCHED_PRIO){
    lock_release_prio(lock);
  }else{
    sema_up(&lock->semaphore);
    lock->holder = NULL;
  }
}

/* 优先级策略下的释放 */
static void lock_release_prio(struct lock* lock){
  enum intr_level old_level;
  struct thread_priority* t_p;

  //如果还未启动捐赠单元就普通操作
  if(list_empty(&priorities_list)){
    sema_up(&lock->semaphore);
    lock->holder = NULL;
  }else{
    t_p = thread_unit(thread_current());
    old_level = intr_disable();

    lock->holder = NULL;
    sema_up(&lock->semaphore);
    if(!list_empty(&t_p->locks))
      end_offer(lock, t_p);

    intr_set_level(old_level);
  }

}

/* Returns true if the current thread holds LOCK, false
   otherwise.  (Note that testing whether some other thread holds
   a lock would be racy.) */
bool lock_held_by_current_thread(const struct lock* lock) {
  ASSERT(lock != NULL);

  return lock->holder == thread_current();
}

/* Initializes a readers-writers lock */
void rw_lock_init(struct rw_lock* rw_lock) {
  lock_init(&rw_lock->lock);
  cond_init(&rw_lock->read);
  cond_init(&rw_lock->write);
  rw_lock->AR = rw_lock->WR = rw_lock->AW = rw_lock->WW = 0;
}

/* Acquire a writer-centric readers-writers lock */
void rw_lock_acquire(struct rw_lock* rw_lock, bool reader) {
  // Must hold the guard lock the entire time
  lock_acquire(&rw_lock->lock);

  if (reader) {
    // Reader code: Block while there are waiting or active writers
    while ((rw_lock->AW + rw_lock->WW) > 0) {
      rw_lock->WR++;
      cond_wait(&rw_lock->read, &rw_lock->lock);
      rw_lock->WR--;
    }
    rw_lock->AR++;
  } else {
    // Writer code: Block while there are any active readers/writers in the system
    while ((rw_lock->AR + rw_lock->AW) > 0) {
      rw_lock->WW++;
      cond_wait(&rw_lock->write, &rw_lock->lock);
      rw_lock->WW--;
    }
    rw_lock->AW++;
  }

  // Release guard lock
  lock_release(&rw_lock->lock);
}

/* Release a writer-centric readers-writers lock */
void rw_lock_release(struct rw_lock* rw_lock, bool reader) {
  // Must hold the guard lock the entire time
  lock_acquire(&rw_lock->lock);

  if (reader) {
    // Reader code: Wake any waiting writers if we are the last reader
    rw_lock->AR--;
    if (rw_lock->AR == 0 && rw_lock->WW > 0)
      cond_signal(&rw_lock->write, &rw_lock->lock);
  } else {
    // Writer code: First try to wake a waiting writer, otherwise all waiting readers
    rw_lock->AW--;
    if (rw_lock->WW > 0)
      cond_signal(&rw_lock->write, &rw_lock->lock);
    else if (rw_lock->WR > 0)
      cond_broadcast(&rw_lock->read, &rw_lock->lock);
  }

  // Release guard lock
  lock_release(&rw_lock->lock);
}

/* One semaphore in a list. */
struct semaphore_elem {
  struct list_elem elem;      /* List element. */
  struct semaphore semaphore; /* This semaphore. */
};

static struct semaphore *cond_max_sema(struct condition* cond);

/* Initializes condition variable COND.  A condition variable
   allows one piece of code to signal a condition and cooperating
   code to receive the signal and act upon it. */
void cond_init(struct condition* cond) {
  ASSERT(cond != NULL);

  list_init(&cond->waiters);
}

/* Atomically releases LOCK and waits for COND to be signaled by
   some other piece of code.  After COND is signaled, LOCK is
   reacquired before returning.  LOCK must be held before calling
   this function.

   The monitor implemented by this function is "Mesa" style, not
   "Hoare" style, that is, sending and receiving a signal are not
   an atomic operation.  Thus, typically the caller must recheck
   the condition after the wait completes and, if necessary, wait
   again.

   A given condition variable is associated with only a single
   lock, but one lock may be associated with any number of
   condition variables.  That is, there is a one-to-many mapping
   from locks to condition variables.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but interrupts will be turned back on if
   we need to sleep. */
void cond_wait(struct condition* cond, struct lock* lock) {
  struct semaphore_elem waiter;

  ASSERT(cond != NULL);
  ASSERT(lock != NULL);
  ASSERT(!intr_context());
  ASSERT(lock_held_by_current_thread(lock));

  sema_init(&waiter.semaphore, 0);
  list_push_back(&cond->waiters, &waiter.elem);
  lock_release(lock);
  sema_down(&waiter.semaphore);
  lock_acquire(lock);
}

/* If any threads are waiting on COND (protected by LOCK), then
   this function signals one of them to wake up from its wait.
   LOCK must be held before calling this function.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to signal a condition variable within an
   interrupt handler. */
void cond_signal(struct condition* cond, struct lock* lock UNUSED) {
  ASSERT(cond != NULL);
  ASSERT(lock != NULL);
  ASSERT(!intr_context());
  ASSERT(lock_held_by_current_thread(lock));

  if (!list_empty(&cond->waiters))
    sema_up(cond_max_sema(cond));
}

static struct semaphore *cond_max_sema(struct condition* cond){
  ASSERT(!list_empty(&cond->waiters));

  struct list* waiters = &cond->waiters;
  struct list_elem *e;
  struct semaphore_elem *sema,*max_sema;
  int prio,max_prio;
  max_prio = -1;
  for(e = list_begin(waiters); e != list_end(waiters); e = list_next(e)){
    sema = list_entry(e, struct semaphore_elem, elem);
    struct semaphore* s = &sema->semaphore;
    prio = list_entry(list_begin(&s->waiters), struct thread, elem)->priority;
    if(prio > max_prio){
      max_sema = sema;
      max_prio = prio;
    }
  }
  list_remove(&max_sema->elem);
  return &max_sema->semaphore;
}

/* Wakes up all threads, if any, waiting on COND (protected by
   LOCK).  LOCK must be held before calling this function.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to signal a condition variable within an
   interrupt handler. */
void cond_broadcast(struct condition* cond, struct lock* lock) {
  ASSERT(cond != NULL);
  ASSERT(lock != NULL);

  while (!list_empty(&cond->waiters))
    cond_signal(cond, lock);
}
