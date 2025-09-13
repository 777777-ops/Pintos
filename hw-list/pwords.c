/*
 * Word count application with one thread per input file.
 *
 * You may modify this file in any way you like, and are expected to modify it.
 * Your solution must read each input file from a separate thread. We encourage
 * you to make as few changes as necessary.
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

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <pthread.h>

#include "word_count.h"
#include "word_helpers.h"

/*
 * main - handle command line, spawning one thread per file.
 */
 
word_count_list_t word_counts;


char* splice(char* sr1, char sr2);

void* threadfun(void* fileName) {
  FILE *file = fopen((char*)fileName,"r");
  int ch;
  char *word = NULL;
  while((ch = fgetc(file)) != EOF){
    if(isalpha(ch)){
      ch = tolower(ch);
      word = splice(word,(char)ch);
    }else{
      if(word && strlen(word) > 1)
        add_word(&word_counts,word);
      word = NULL;
    }
  }
  if(word && strlen(word) > 1)
    add_word(&word_counts,word);


  fclose(file);
}

//sr1必须是堆上静态分配的空间
char* splice(char* sr1, char sr2){
  char *result;
  if(sr1 == NULL){
    result = (char*)malloc(2*sizeof(char));
    result[0] = sr2; 
    result[1] = '\0';
  }else{
    size_t len = strlen(sr1);
    result = realloc(sr1,(len + 1 + 1)*sizeof(char));
    if(result == NULL) 
      exit(-1);
    result[len] = sr2;
    result[len+1] = '\0';
  }
  return result;
}

int main(int argc, char* argv[]) {
  /* Create the empty data structure. */
  init_words(&word_counts);

  if (argc <= 1) {
    /* Process stdin in a single thread. */
    count_words(&word_counts, stdin);
  } else {
    /* TODO */
    int nthreads = argc - 1;
    pthread_t threads[nthreads];
    for (int t = 0; t < nthreads; t++) {
      int rc = pthread_create(&threads[t], NULL, threadfun, (void*)argv[t+1]);
      if (rc) {
        printf("ERROR; return code from pthread_create() is %d\n", rc);
        exit(-1);
      }
      
       
    }

    for (int t = 0; t < nthreads; t++) {
      pthread_join(threads[t], NULL);
    }
  }

  /* Output final result of all threads' work. */
  wordcount_sort(&word_counts, less_count);
  fprint_words(&word_counts, stdout);
  return 0;
}
