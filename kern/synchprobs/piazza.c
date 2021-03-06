/*
 * Copyright (c) 2013
 *	The President and Fellows of Harvard College.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE UNIVERSITY OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/* Correctness criteria:
- There should be NINSTRUCTORS * NCYCLES lines of output in the format:
[ 1] aaaaaaaaaa
  Using the default values in common.h, this would be 500 lines of output.
- The program should not panic, least of all because an answer changed when a
  student was reading an answer.
- There are no restrictions on when a question (indicated by the bracketed
  number in the output) can be answered (ie the output can have the questions
  show up in any order and repeated in any way). However, first time a question
  is answered, the answer must be "aaaaaaaaaa" and subsequent answers must be
  the next uncapitalized character in the alphabet, looping back to "aaaaaaaaaa"
  after "zzzzzzzzzz".

  By running pz and carefully parsing the output, we've concluded that all of
  the above correctness criteria are met, and so the synchronization problem
  has been solved correctly using reader/writer locks. The way the parsing was
  performed was by modifying that output such that it took a csv format that we
  could feed into Excel, sort by question number, and easily eyeball for any
  violations.
*/

/**
 * Driver code for the Piazza synch problem.
 */

#include <types.h>
#include <lib.h>
#include <thread.h>
#include <test.h>
#include <generic/random.h>
#include <synch.h>

#include "common.h"

/**
 * struct piazza_question - Object representing a question on Piazza.
 */
struct piazza_question {
  char *pq_answer;
  struct lock *mutex;
  struct cv *readerQ;
  struct cv *writerQ;
  int readers;
  int writers;
  int active_writer;
};

struct piazza_question *questions[NANSWERS] = { 0 };

struct lock *creation_lock[NANSWERS];
struct semaphore *driver_sem;
struct lock *thread_count_lock;

int finished_thread_count;

static void
piazza_print(int id)
{
  KASSERT(id < NANSWERS);

  kprintf("[%2d] %s\n", id, questions[id]->pq_answer);
}

/**
 * student - Piazza answer-reading thread.
 *
 * The student threads repeatedly choose a random Piazza question for which to
 * read the instructors' answer.  Unlike CS 161 students, these students are
 * very slow and after reading each character, need some time to rest and thus
 * deschedule themselves.
 *
 * You may add as much synchronization code as you wish to this function, but
 * you may not change the way the students read.
 */
static void
student(void *p, unsigned long which)
{
  (void)p;

  int i, n;
  char letter, *pos;

  for (i = 0; i < NCYCLES; ++i) {
    // Choose a random Piazza question.
    n = random() % NANSWERS;

    // If the instructors haven't seen the question yet, try again.
    if (questions[n] == NULL) {
      --i;
      continue;
    }
    lock_acquire(creation_lock[n]);
    lock_release(creation_lock[n]);

    // Set up reader lock
    lock_acquire(questions[n]->mutex);
    while(!(questions[n]->writers==0))
      cv_wait(questions[n]->readerQ, questions[n]->mutex);
    questions[n]->readers++;
    lock_release(questions[n]->mutex);

    /* Start read */
    pos = questions[n]->pq_answer;
    letter = *pos;

    // Read the answer slowly.
    while (*(++pos) == letter) {
      thread_yield();
    }

    // If the answer changes while we're reading it, panic!  Panic so much that
    // the kernel explodes.
    if (*pos != '\0') {
      panic("[%d:%d] Inconsistent answer!\n", (int)which, n); //TODO figure out why
    }

    /* End read */

    // Close reader lock
    lock_acquire(questions[n]->mutex);
    questions[n]->readers--;
    if(questions[n]->readers == 0)
      cv_signal(questions[n]->writerQ, questions[n]->mutex);
    lock_release(questions[n]->mutex);
  }
  // Exiting thread
  lock_acquire(thread_count_lock);
  if(++finished_thread_count == NSTUDENTS + NINSTRUCTORS)
    V(driver_sem);
  lock_release(thread_count_lock);
}

/**
 * instructor - Piazza answer-editing thread.
 *
 * Each instructor thread should, for NCYCLES iterations, choose a random
 * Piazza question and then update the answer.  The answer should always
 * consist of a lowercase alphabetic character repeated 10 times, e.g.,
 *
 *    "aaaaaaaaaa"
 *
 * and each update should increment all ten characters (cycling back to a's
 * from z's if a question is updated enough times).
 *
 * After each update, (including the first update, in which you should create
 * the question and initialize the answer to all a's), the instructor should
 * print the answer string using piazza_print().
 *
 * TODO: Implement this.
 */
static void
instructor(void *p, unsigned long which)
{
  (void)p;
  (void)which;

  int i, n;
  char letter, *pos;

  for (i = 0; i < NCYCLES; ++i) {
    // Choose a random Piazza question.
    n = random() % NANSWERS;

    // If first instructor to see the question, initalize answers
    lock_acquire(creation_lock[n]);
    if (questions[n] == NULL) {
      questions[n] = kmalloc(sizeof(struct piazza_question));
      questions[n]->mutex = lock_create("mutex");
      questions[n]->readerQ = cv_create("readerQ");
      questions[n]->writerQ = cv_create("writerQ");
      questions[n]->readers = 0;
      questions[n]->writers = 0;
      questions[n]->active_writer = 0;

      const char *answer = "aaaaaaaaaa"; //TODO: have const here?
      questions[n]->pq_answer = kstrdup(answer);      

      lock_release(creation_lock[n]);

      // Print submitted answer
      piazza_print(n);
    }

    // Not the first instructor
    else{
      lock_release(creation_lock[n]);

      // Set up writer lock
      lock_acquire(questions[n]->mutex);
      questions[n]->writers++;
      while(!((questions[n]->readers == 0) && (questions[n]->active_writer == 0)))
        cv_wait(questions[n]->writerQ, questions[n]->mutex);
      questions[n]->active_writer++;
      lock_release(questions[n]->mutex);

      /* Start write */
      pos = questions[n]->pq_answer;
      letter = *pos;

      // Update answer
      if(letter != 'z'){
        while (*(pos) == letter) {
          (*pos)++;
          pos++;
        }        
      }

      // Loop answer back to A's
      else{
        while (*(pos) == letter) {
          *pos = 'a';
          pos++;
        }
      }
    
      // Print submitted answer
      piazza_print(n);

      /* End write */

      // Clean up writer lock
      lock_acquire(questions[n]->mutex);
      questions[n]->active_writer--;
      questions[n]->writers--;
      if(questions[n]->writers==0)
        cv_broadcast(questions[n]->readerQ, questions[n]->mutex);
      else
        cv_signal(questions[n]->writerQ, questions[n]->mutex);
      lock_release(questions[n]->mutex);
    }
  }
  // Exiting thread
  lock_acquire(thread_count_lock);
  if(++finished_thread_count == NSTUDENTS + NINSTRUCTORS){
    lock_release(thread_count_lock);
    V(driver_sem);
  }
  else
    lock_release(thread_count_lock);
}

/**
 * piazza - Piazza synch problem driver routine.
 *
 * You may modify this function to initialize any synchronization primitives
 * you need; however, any other data structures you need to solve the problem
 * must be handled entirely by the forked threads (except for some freeing at
 * the end).
 *
 * Make sure you don't leak any kernel memory!  Also, try to return the test to
 * its original state so it can be run again.
 */
int
piazza(int nargs, char **args)
{
  int i;

  (void)nargs;
  (void)args;

  // Initalize locks and globals
  for(i=0; i<NANSWERS; i++)
    creation_lock[i] = lock_create("creation lock");
  driver_sem = sem_create("driver semaphore", 0);
  thread_count_lock = lock_create("thread count lock");
  finished_thread_count = 0;

  for (i = 0; i < NSTUDENTS; ++i) {
    thread_fork_or_panic("student", student, NULL, i, NULL);
  }
  for (i = 0; i < NINSTRUCTORS; ++i) {
    thread_fork_or_panic("instructor", instructor, NULL, i, NULL);
  }

  // Wait for task to finish
  P(driver_sem);

  // Cleanup
  for(i=0; i<NANSWERS; i++){
    lock_destroy(questions[i]->mutex);
    cv_destroy(questions[i]->readerQ);
    cv_destroy(questions[i]->writerQ);
    kfree(questions[i]->pq_answer);
    kfree(questions[i]);
    questions[i] = NULL;

    lock_destroy(creation_lock[i]);
  }
  sem_destroy(driver_sem);
  lock_destroy(thread_count_lock);

  return 0;
}
