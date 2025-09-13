/*
 * Implementation of the word_count interface using Pintos lists and pthreads.
 *
 * You may modify this file, and are expected to modify it.
 */

/*
 * Copyright © 2021 University of California, Berkeley
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef PINTOS_LIST
#error "PINTOS_LIST must be #define'd when compiling word_count_lp.c"
#endif

#ifndef PTHREADS
#error "PTHREADS must be #define'd when compiling word_count_lp.c"
#endif

#include "word_count.h"
#include "debug.h"

void init_words(word_count_list_t* wclist) { /* TODO */

  ASSERT(wclist != NULL);
  list_init (&wclist->lst);
  pthread_mutex_init(&wclist->lock, NULL); // 第二个参数为NULL表示使用默认属性
}

size_t len_words(word_count_list_t* wclist) {
  /* TODO */
  
  ASSERT(wclist != NULL);
  struct list_elem* e;
  size_t wcount = 0;
  //遍历
  pthread_mutex_lock(&wclist->lock); // 获取锁
  for (e = list_begin(&(wclist->lst)); e != list_end(&(wclist->lst)); e = list_next(e)){
    wcount += list_entry(e,struct word_count, elem)->count;
  }
  pthread_mutex_unlock(&wclist->lock); // 释放锁
  return wcount;
}

word_count_t* find_word(word_count_list_t* wclist, char* word) {
  /* TODO */
   ASSERT(wclist != NULL);
  struct list_elem* e;

  //pthread_rwlock_rdlock(&rwlock); // 获取读锁
  //遍历
  for (e = list_begin(&(wclist->lst)); e != list_end(&(wclist->lst)); e = list_next(e)){
    word_count_t* prt = list_entry(e,struct word_count, elem);
    if(!strcmp(prt->word,word)){
      return prt;
    }
  }
  //pthread_rwlock_unlock(&rwlock); // 释放读锁
  return NULL;
}

word_count_t* add_word(word_count_list_t* wclist, char* word) {
  /* TODO */

  pthread_mutex_lock(&wclist->lock);  // 获取锁

  word_count_t* nw = find_word(wclist, word);
  if(nw != NULL){
     nw->count ++;
     pthread_mutex_unlock(&wclist->lock); // 释放写锁
     return nw;
  } 

  nw = (word_count_t*)malloc(sizeof(struct word_count));
  nw->count = 1; nw->word = word; 
  list_insert(list_tail(&(wclist->lst)),&nw->elem);

  pthread_mutex_unlock(&wclist->lock); // 释放写锁
  return nw;
}

void fprint_words(word_count_list_t* wclist, FILE* outfile) {
  /* TODO */
  /* Please follow this format: fprintf(<file>, "%i\t%s\n", <count>, <word>); */

  ASSERT(wclist != NULL);
  struct list_elem* e;

  pthread_mutex_lock(&wclist->lock); // 获取读锁
  //遍历
  for (e = list_begin(&(wclist->lst)); e != list_end(&(wclist->lst)); e = list_next(e)){
    word_count_t* wc = list_entry(e,struct word_count, elem);
    fprintf(outfile,"%i\t%s\n",wc->count,wc->word);
  }
  pthread_mutex_unlock(&wclist->lock); // 释放读锁
}

static bool less_list(const struct list_elem* ewc1, const struct list_elem* ewc2, void* aux) {
  /* TODO */

  word_count_t* wc1 = list_entry(ewc1,struct word_count, elem);
  word_count_t* wc2 = list_entry(ewc2,struct word_count, elem);
  bool (*less)(const word_count_t*, const word_count_t*) = aux;
  return less(wc1, wc2);
}

void wordcount_sort(word_count_list_t* wclist,
                    bool less(const word_count_t*, const word_count_t*)) {
  pthread_mutex_lock(&wclist->lock); // 获取写锁
  list_sort(&(wclist->lst), less_list, less);
  pthread_mutex_unlock(&wclist->lock); // 释放读锁

}