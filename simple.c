/**
 * This software is released into the public domain.
 */

//#define NDEBUG

#define _DEFAULT_SOURCE
#define USE_RMV 1

#if USE_RMV
#define RMV rmv
#else
#define RMV rmv_non_stalling
#endif

#include "mpscfifo.h"
#include "msg_pool.h"
#include "diff_timespec.h"
#include "dpf.h"

#include <sys/types.h>
#include <pthread.h>

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <semaphore.h>

/**
 * We pass pointers in Msg_t.arg2 which is a uint64_t,
 * verify a void* fits.
 * TODO: sizeof(uint64_t) should be sizeof(Msg_t.arg2), how to do that?
 */
_Static_assert(sizeof(uint64_t) >= sizeof(void*), "Expect sizeof uint64_t >= sizeof void*");

_Atomic(uint64_t) gTick = 0;

bool simple(void) {
  MpscFifo_t cmdFifo;
  MsgPool_t pool;

  printf(LDR "simple:+\n", ldr());

  printf(LDR "simple: init pool=%p\n", ldr(), &pool);
  bool error = MsgPool_init(&pool, 2);
  if (error) {
    printf(LDR "simple: ERROR unable to create msgs for pool\n", ldr());
    goto done;
  }

  // Init cmdFifo
  printf(LDR "simple: init cmdFifo=%p\n", ldr(), &cmdFifo);
  initMpscFifo(&cmdFifo);

  printf(LDR "simple: remove from empty cmdFifo=%p\n", ldr(), &cmdFifo);
  Msg_t* pMsg = rmv(&cmdFifo);
  if (pMsg != NULL) {
    printf(LDR "simple: ERROR expected pMsg=%p == NULL\n", ldr(), pMsg);
    error |= true;
  }
  
  printf(LDR "simple: add a message to empty cmdFifo=%p\n", ldr(), &cmdFifo);
  pMsg = MsgPool_get_msg(&pool);
  pMsg->arg1 = 1;
  pMsg->arg2 = 2;
  add(&cmdFifo, pMsg);

  printf(LDR "simple: remove from with one item in cmdFifo=%p\n", ldr(), &cmdFifo);
  pMsg = rmv(&cmdFifo);
  if (pMsg == NULL) {
    printf(LDR "simple: ERROR expected pMsg=%p != NULL\n", ldr(), pMsg);
    error |= true;
  } else if (pMsg->arg1 != 1 || pMsg->arg2 != 2) {
    printf(LDR "simple: ERROR expected pMsg=%p && arg1=%lu != 1 && arg2=%lu != 2\n",
        ldr(), pMsg, pMsg->arg1, pMsg->arg2);
    error |= true;
  }
  
  printf(LDR "simple: add a message to empty cmdFifo=%p\n", ldr(), &cmdFifo);
  add(&cmdFifo, pMsg);

  printf(LDR "simple: add a message to non-empty cmdFifo=%p\n", ldr(), &cmdFifo);
  Msg_t* pMsg2 = MsgPool_get_msg(&pool);
  pMsg2->arg1 = 3;
  pMsg2->arg2 = 4;
  add(&cmdFifo, pMsg2);

  printf(LDR "simple: remove msg1 from cmdFifo=%p\n", ldr(), &cmdFifo);
  pMsg = rmv(&cmdFifo);
  if (pMsg == NULL) {
    printf(LDR "simple: ERROR expected pMsg=%p != NULL\n", ldr(), pMsg);
    error |= true;
  } else if (pMsg->arg1 != 1 || pMsg->arg2 != 2) {
    printf(LDR "simple: ERROR expected pMsg=%p arg1=%lu != 1 && arg2=%lu != 2\n",
        ldr(), pMsg, pMsg->arg1, pMsg->arg2);
    error |= true;
  }
  
  printf(LDR "simple: remove msg2 from cmdFifo=%p\n", ldr(), &cmdFifo);
  pMsg = rmv(&cmdFifo);
  if (pMsg == NULL) {
    printf(LDR "simple: ERROR expected pMsg=%p != NULL\n", ldr(), pMsg);
    error |= true;
  } else if (pMsg->arg1 != 3 || pMsg->arg2 != 4) {
    printf(LDR "simple: ERROR expected pMsg=%p arg1=%lu != 3 && arg2=%lu != 4\n",
        ldr(), pMsg, pMsg->arg1, pMsg->arg2);
    error |= true;
  }
  
  
  printf(LDR "simple: remove from empty cmdFifo=%p\n", ldr(), &cmdFifo);
  pMsg = rmv(&cmdFifo);
  if (pMsg != NULL) {
    printf(LDR "simple: ERROR expected pMsg=%p == NULL\n", ldr(), pMsg);
    error |= true;
  }
  
done:
  printf(LDR "simple:-error=%u\n\n", ldr(), error);

  return error;
}


bool perf(const uint64_t loops) {
  struct timespec time_start;
  struct timespec time_stop;
  MpscFifo_t cmdFifo;
  MsgPool_t pool;

  printf(LDR "perf:+loops=%lu\n", ldr(), loops);

  printf(LDR "simple: init pool=%p\n", ldr(), &pool);
  bool error = MsgPool_init(&pool, 4); // One more for the cmdFifo
  if (error) {
    printf(LDR "perf: ERROR unable to create msgs for pool\n", ldr());
    goto done;
  }

  // Init cmdFifo
  DPF(LDR "perf: cmdFifo=%p\n", ldr(), &cmdFifo);
  initMpscFifo(&cmdFifo);
  
  DPF(LDR "perf: remove from empty cmdFifo=%p\n", ldr(), &cmdFifo);
  Msg_t* pMsg = rmv(&cmdFifo);
  if (pMsg != NULL) {
    printf(LDR "perf: ERROR expected pMsg=%p == NULL\n", ldr(), pMsg);
    error |= true;
  }

  Msg_t* msg = MsgPool_get_msg(&pool);
  clock_gettime(CLOCK_REALTIME, &time_start);
  for (uint64_t i = 0; i < loops; i++) {
    add(&cmdFifo, msg);
    msg = rmv(&cmdFifo);
  }
  clock_gettime(CLOCK_REALTIME, &time_stop);
  
  double processing_ns = diff_timespec_ns(&time_stop, &time_start);
  printf(LDR "perf: add_rmv from empty fifo  processing=%.3fs\n", ldr(), processing_ns / ns_flt);
  double ops_per_sec = (loops * ns_flt) / processing_ns;
  printf(LDR "perf: add rmv from empty fifo ops_per_sec=%.3f\n", ldr(), ops_per_sec);
  double ns_per_op = (float)processing_ns / (double)loops;
  printf(LDR "perf: add rmv from empty fifo   ns_per_op=%.1fns\n", ldr(), ns_per_op);

  Msg_t* msg2 = MsgPool_get_msg(&pool);
  add(&cmdFifo, msg2);

  clock_gettime(CLOCK_REALTIME, &time_start);
  for (uint64_t i = 0; i < loops; i++) {
    add(&cmdFifo, msg);
    msg = rmv(&cmdFifo);
  }
  clock_gettime(CLOCK_REALTIME, &time_stop);
  
  processing_ns = diff_timespec_ns(&time_stop, &time_start);
  printf(LDR "perf: add_rmv from non-empty fifo  processing=%.3fs\n", ldr(), processing_ns / ns_flt);
  ops_per_sec = (loops * ns_flt) / processing_ns;
  printf(LDR "perf: add rmv from non-empty fifo ops_per_sec=%.3f\n", ldr(), ops_per_sec);
  ns_per_op = (float)processing_ns / (double)loops;
  printf(LDR "perf: add rmv from non-empty fifo   ns_per_op=%.1fns\n", ldr(), ns_per_op);

done:
  printf(LDR "perf:-error=%u\n\n", ldr(), error);

  return error;
}

int main(int argc, char* argv[]) {
  bool error = false;

  if (argc != 2) {
    printf("Usage:\n");
    printf(" %s loops\n", argv[0]);
    return 1;
  }

  u_int64_t loops;
  sscanf(argv[1], "%lu", &loops);
  printf("test loops=%lu\n", loops);

  error |= simple();
  error |= perf(loops);

  if (!error) {
    printf("Success\n");
  }

  return error ? 1 : 0;
}
