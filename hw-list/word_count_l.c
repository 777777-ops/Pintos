/*
 * Implementation of the word_count interface using Pintos lists.
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
#error "PINTOS_LIST must be #define'd when compiling word_count_l.c"
#endif

#include "word_count.h"
#include "debug.h"

void init_words(word_count_list_t* wclist) { /* TODO */

  ASSERT(wclist != NULL);
  list_init (wclist);
}

size_t len_words(word_count_list_t* wclist) {
  
  ASSERT(wclist != NULL);
  struct list_elem* e;
  size_t wcount = 0;
  //遍历
  for (e = list_begin(wclist); e != list_end(wclist); e = list_next(e))
  {
    wcount += list_entry(e,struct word_count, elem)->count;
  }
  return wcount;
}

word_count_t* find_word(word_count_list_t* wclist, char* word) {
  /* TODO */
  ASSERT(wclist != NULL);
  struct list_elem* e;
  //遍历
  for (e = list_begin(wclist); e != list_end(wclist); e = list_next(e))
  {
    word_count_t* prt = list_entry(e,struct word_count, elem);
    if(!strcmp(prt->word,word))
      return prt;
  }
  return NULL;
}

word_count_t* add_word(word_count_list_t* wclist, char* word) {
  /* TODO */

  word_count_t* nw = find_word(wclist, word);
  if(nw != NULL){
    nw->count ++;
    return nw;
  } 

  nw = (word_count_t*)malloc(sizeof(struct word_count));
  nw->count = 1; nw->word = word; 
  list_insert(list_tail(wclist),&nw->elem);
  return nw;
}

void fprint_words(word_count_list_t* wclist, FILE* outfile) {
  /* TODO */
  /* Please follow this format: fprintf(<file>, "%i\t%s\n", <count>, <word>); */

  ASSERT(wclist != NULL);
  struct list_elem* e;
  //遍历
  for (e = list_begin(wclist); e != list_end(wclist); e = list_next(e)){
    word_count_t* wc = list_entry(e,struct word_count, elem);
    fprintf(outfile,"%i\t%s\n",wc->count,wc->word);
  }
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
  list_sort(wclist, less_list, less);
}
