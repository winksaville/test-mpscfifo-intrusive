/*
 * This software is released into the public domain.
 *
 * A MpscFifo is a wait free/thread safe multi-producer
 * single consumer first in first out queue. This algorithm
 * is from Dimitry Vyukov's non intrusive MPSC code here:
 *   http://www.1024cores.net/home/lock-free-algorithms/queues/intrusive-mpsc-node-based-queue
 *
 * The fifo has a head and tail, the elements are added
 * to the head of the queue and removed from the tail.
 */

#define NDEBUG

#define _DEFAULT_SOURCE

#define DELAY 0

#include "mpscfifo.h"
#include "dpf.h"

#include <sys/types.h>
#include <pthread.h>

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <unistd.h>

/**
 * @see mpscfifo.h
 */
MpscFifo_t *initMpscFifo(MpscFifo_t *pQ) {
  DPF(LDR "initMpscFifo:*pQ=%p\n", ldr(), pQ);
  pQ->pHead = &pQ->stub;
  pQ->pTail = &pQ->stub;
  pQ->count = 0;
  pQ->stub.pNext = NULL;
  pQ->stub.pPool = NULL;
  pQ->stub.pRspQ = NULL;
  pQ->stub.arg1 = 11110001;
  pQ->stub.arg2 = 11110002;;
  return pQ;
}

/**
 * @see mpscfifo.h
 */
void deinitMpscFifo(MpscFifo_t *pQ) {
  pQ->pHead = NULL;
  pQ->pTail = NULL;
}

/**
 * @see mpscifo.h
 */
void add(MpscFifo_t *pQ, Msg_t *pMsg) {
  DPF(LDR "add:+pQ=%p count=%d msg=%p pool=%p arg1=%lu arg2=%lu\n", ldr(), pQ, pQ->count, pMsg, pMsg->pPool, pMsg->arg1, pMsg->arg2);
  DPF(LDR "add: pQ=%p count=%d pHead=%p pHead->pNext=%p pTail=%p pTail->pNext=%p\n", ldr(), pQ, pQ->count, pQ->pHead, pQ->pHead->pNext, pQ->pTail, pQ->pTail->pNext);
#if USE_ATOMIC_TYPES
  pMsg->pNext = NULL;
  void** ptr_pHead = (void*)&pQ->pHead;
  Msg_t* pPrev = __atomic_exchange_n(ptr_pHead, pMsg, __ATOMIC_SEQ_CST);
  // rmv will stall spinning if preempted at this critical spot

#if DELAY != 0
  usleep(DELAY);
#endif

  pPrev->pNext = pMsg;
  //mfence();
#else
  pMsg->pNext = NULL;
  Msg_t** ptr_pHead = &pQ->pHead;
  Msg_t* pPrev = __atomic_exchange_n(ptr_pHead, pMsg, __ATOMIC_ACQ_REL); //SEQ_CST);
  // rmv will stall spinning if preempted at this critical spot

#if DELAY != 0
  usleep(DELAY);
#endif

  Msg_t** ptr_pNext = &pPrev->pNext;
  __atomic_store_n(ptr_pNext, pMsg, __ATOMIC_RELEASE); //SEQ_CST);
#endif
  if (pMsg != &pQ->stub) {
    // Don't count adding the stub
    pQ->count += 1;
  }
  DPF(LDR "add: pQ=%p count=%d pPrev=%p pPrev->pNext=%p\n", ldr(), pQ, pQ->count, pPrev, pPrev->pNext);
  DPF(LDR "add: pQ=%p count=%d pHead=%p pHead->pNext=%p pTail=%p pTail->pNext=%p\n", ldr(), pQ, pQ->count, pQ->pHead, pQ->pHead->pNext, pQ->pTail, pQ->pTail->pNext);
  DPF(LDR "add:-pQ=%p count=%d msg=%p pool=%p arg1=%lu arg2=%lu\n", ldr(), pQ, pQ->count, pMsg, pMsg->pPool, pMsg->arg1, pMsg->arg2);
}

/**
 * Same as add above excepts it asserts(pMsg->pPool == pQ), i.e the message
 * being returned belongs the the pool. This maybe used by multiple
 * entities on the same or different thread. This will never
 * block as it is a wait free algorithm.
 */
void ret(MpscFifo_t *pQ, Msg_t *pMsg) {
  DPF(LDR "ret:+pQ=%p count=%d msg=%p pool=%p arg1=%lu arg2=%lu\n", ldr(), pQ, pQ->count, pMsg, pMsg->pPool, pMsg->arg1, pMsg->arg2);
  DPF(LDR "ret: pQ=%p count=%d pHead=%p pHead->pNext=%p pTail=%p pTail->pNext=%p\n", ldr(), pQ, pQ->count, pQ->pHead, pQ->pHead->pNext, pQ->pTail, pQ->pTail->pNext);
  assert(pMsg->pPool == pQ);
#if USE_ATOMIC_TYPES
  pMsg->pNext = NULL;
  void** ptr_pHead = (void*)&pQ->pHead;
  Msg_t* pPrev = __atomic_exchange_n(ptr_pHead, pMsg, __ATOMIC_SEQ_CST);
  // rmv will stall spinning if preempted at this critical spot

#if DELAY != 0
  usleep(DELAY);
#endif

  pPrev->pNext = pMsg;
  //mfence();
#else
  pMsg->pNext = NULL;
  Msg_t** ptr_pHead = &pQ->pHead;
  Msg_t* pPrev = __atomic_exchange_n(ptr_pHead, pMsg, __ATOMIC_ACQ_REL); //SEQ_CST);
  // rmv will stall spinning if preempted at this critical spot


#if DELAY != 0
  usleep(DELAY);
#endif

  Msg_t** ptr_pNext = &pPrev->pNext;
  __atomic_store_n(ptr_pNext, pMsg, __ATOMIC_RELEASE); //SEQ_CST);
#endif
  if (pMsg != &pQ->stub) {
    // Don't count adding the stub
    pQ->count += 1;
  }
  DPF(LDR "ret: pQ=%p count=%d pPrev=%p pPrev->pNext=%p\n", ldr(), pQ, pQ->count, pPrev, pPrev->pNext);
  DPF(LDR "ret: pQ=%p count=%d pHead=%p pHead->pNext=%p pTail=%p pTail->pNext=%p\n", ldr(), pQ, pQ->count, pQ->pHead, pQ->pHead->pNext, pQ->pTail, pQ->pTail->pNext);
  DPF(LDR "ret:-pQ=%p count=%d msg=%p pool=%p arg1=%lu arg2=%lu\n", ldr(), pQ, pQ->count, pMsg, pMsg->pPool, pMsg->arg1, pMsg->arg2);
}

/**
 * @see mpscifo.h
 */
Msg_t *rmv_non_stalling(MpscFifo_t *pQ) {
#if USE_ATOMIC_TYPES
  Msg_t* pTail = pQ->pTail;
  Msg_t* pNext = pTail->pNext;
  DPF(LDR "rmv_non_stalling:0+pQ=%p count=%d pTail=%p pNext=%p\n", ldr(), pQ, pQ->count, pTail, pNext);
  if (pTail == &pQ->stub) {
    // Nothing has been removed since Q was last empty
    if (pNext == NULL) {
      // Queue is empty
      DPF(LDR "rmv_non_stalling:1-is EMPTY pQ=%p count=%d msg=NULL\n", ldr(), pQ, pQ->count);
      return NULL;
    }
    // Advance tail to real "tail"
    pQ->pTail = pNext;
    pTail = pNext;
    pNext = pNext->pNext;
  }
  if (pNext != NULL) {
    // Not empty and there are more elements
    pQ->pTail = pNext;
    DPF(LDR "rmv_non_stalling:2 got msg fifo has at least 1 more element pQ=%p count=%d\n", ldr(), pQ, pQ->count);
  } else {
    // We've reached the end of the fifo as known by pQ->pTail and
    // two conditions now exist either this is the last element
    // or the producer was preempted.
    Msg_t* pHead = pQ->pHead;
    if (pTail != pHead) {
      // First pHead != pTail then we know we were unlucky and a
      // producer was preempted. So we're not really empty but
      // since this is non-stalling we'll return NULL.
      DPF(LDR "rmv_non_stalling:3 preempted not really 'empty' pQ=%p count=%d msg=NULL\n", ldr(), pQ, pQ->count);
      pTail = NULL;
    } else {
      // Second pHead == pTail then this is the last element
      // in which case we'll add stub to signify the Q if empty.
      // BUT now there are two more conditions.
      add(pQ, &pQ->stub);
      pNext = pTail->pNext;
      if (pNext != NULL) {
        pQ->pTail = pNext;
        DPF(LDR "rmv_non_stalling:4 1 msg and got it, Q is now EMPTY pQ=%p count=%d pTail=%p &pQ->stub=%p\n", ldr(), pQ, pQ->count, pQ->pTail, &pQ->stub);
      } else {
        // While we added our node another thread also added a node
        // and the fifo isn't complete (?). Therefore we can't
        // remove this element until the other producer finishes
        // and since this is non-stalling return NULL.
        pTail = NULL;
        DPF(LDR "rmv_non_stalling:5 1 msg but preempted, is not EMPTY pQ=%p count=%d\n", ldr(), pQ, pQ->count);
      }
    }
  }
  if (pTail != NULL) {
    pTail->pNext = NULL;
    pQ->count -= 1;
    DPF(LDR "rmv_non_stalling:6-got msg pQ=%p count=%d msg=%p pool=%p arg1=%lu arg2=%lu\n", ldr(), pQ, pQ->count, pTail, pTail->pPool, pTail->arg1, pTail->arg2);
  } else {
    DPF(LDR "rmv_non_stalling:7-NO msg pQ=%p count=%d msg=NULL\n", ldr(), pQ, pQ->count);
  }
  return pTail;
#else
  DPF(LDR "rmv_non_stalling: NOT coded\n", ldr());
#endif
}

/**
 * Stall waiting for a producer to finish.
 */
static inline Msg_t* stall(Msg_t* pTail) {
  while (true) {
    Msg_t* pNext = pTail->pNext;
    if (pNext != NULL) {
      return pNext;
    }
    sched_yield();
  }
}

/**
 * @see mpscifo.h
 */
Msg_t *rmv(MpscFifo_t *pQ) {
#if USE_ATOMIC_TYPES
  Msg_t* pTail = pQ->pTail;
  Msg_t* pNext = pTail->pNext;
  DPF(LDR "rmv:0+pQ=%p count=%d pHead=%p pHead->pNext=%p pTail=%p pTail->pNext=%p\n", ldr(), pQ, pQ->count, pQ->pHead, pQ->pHead->pNext, pQ->pTail, pQ->pTail->pNext);
  if (pTail == &pQ->stub) {
    // Nothing has been removed since Q was last empty
    if (pNext == NULL) {
      // Queue is empty
      DPF(LDR "rmv:1-is EMPTY pQ=%p count=%d msg=NULL\n", ldr(), pQ, pQ->count);
      return NULL;
    }
    // Advance tail to real "tail"
    pQ->pTail = pNext;
    pTail = pNext;
    pNext = pTail->pNext;
    DPF(LDR "rmv:2 adv to real 'tail' pQ=%p count=%d pTail=%p pNext=%p\n", ldr(), pQ, pQ->count, pTail, pNext);
  }
  if (pNext != NULL) {
    // Not empty and there are more elements
    pQ->pTail = pNext;
    DPF(LDR "rmv:3 got msg and Q has 1 or more msgs pQ=%p count=%d\n", ldr(), pQ, pQ->count);
  } else {
    // pNext == NULL, we've reached the end of the fifo as known by pQ->pTail and
    // two conditions now exist, either this is the last element or a producer
    // was preempted and pNext hasn't yet been updated.
    DPF(LDR "rmv:4 pNext == NULL, last element or preempted pQ=%p count=%d\n", ldr(), pQ, pQ->count);
    Msg_t* pHead = pQ->pHead;
    if (pTail != pHead) {
      // First pHead != pTail then we know we were unlucky and a
      // producer was preempted while adding and left pNext == NULL.
      // Therefore we'll stall until the producer finishes and it
      // updates pTail->pNext to non-null and then we'll return pTail.
      DPF(LDR "rmv:5 got msg before stalling, Q has 1 or more msgs pQ=%p count=%d\n", ldr(), pQ, pQ->count);
      pNext = stall(pTail);
      DPF(LDR "rmv:6 got msg after stalling, Q has 1 or more msgs pQ=%p count=%d pNext=%p\n", ldr(), pQ, pQ->count, pNext);
    } else {
      // Second pHead == pTail then this is the last element
      // in which case we'll add stub to signify the Q if empty.
      DPF(LDR "rmv:7 1 msg, add stub to pQ=%p\n", ldr(), pQ);
      add(pQ, &pQ->stub);
      pNext = pTail->pNext;
      DPF(LDR "rmv:8 1 msg, added stub to pQ=%p count=%d new pNext=%p\n", ldr(), pQ, pQ->count, pNext);
      if (pNext != NULL) {
        pQ->pTail = pNext;
        DPF(LDR "rmv:9 1 msg and got it, Q is now EMPTY pQ=%p count=%d pTail=%p &pQ->stub=%p pNext=%p\n", ldr(), pQ, pQ->count, pQ->pTail, &pQ->stub, pNext);
      } else {
        // While we added our stub node another thread also adding a node
        // and it was preempted and eaving pNext == NULL. So we'll stall
        // until the producer finishes and it updates pTail->pNext
        // to non-null and then we'll return pTail.
        DPF(LDR "rmv:A 1 msg but preempted, before stalling, is not EMPTY pQ=%p count=%d\n", ldr(), pQ, pQ->count);
        pNext = stall(pTail);
        DPF(LDR "rmv:B 1 msg but preempted, after stalling, is not EMPTY pQ=%p count=%d pNext=%p\n", ldr(), pQ, pQ->count, pNext);
      }
    }
  }
  if (pTail != NULL) {
    pTail->pNext = NULL;
    pQ->count -= 1;
    DPF(LDR "rmv:C got msg pQ=%p count=%d pHead=%p pHead->pNext=%p pTail=%p pTail->pNext=%p\n", ldr(), pQ, pQ->count, pQ->pHead, pQ->pHead->pNext, pQ->pTail, pQ->pTail->pNext);
    DPF(LDR "rmv:D got msg pQ=%p count=%d nxt=%p pool=%p pNext=%p arg1=%lu arg2=%lu\n", ldr(), pQ, pQ->count, pNext, pNext->pPool, pNext->pNext, pNext->arg1, pNext->arg2);
    DPF(LDR "rmv:E-got msg pQ=%p count=%d msg=%p pool=%p pNext=%p arg1=%lu arg2=%lu\n", ldr(), pQ, pQ->count, pTail, pTail->pPool, pTail->pNext, pTail->arg1, pTail->arg2);
  } else {
    DPF(LDR "rmv:F-NO msg pQ=%p count=%d msg=NULL\n", ldr(), pQ, pQ->count);
  }
  return pTail;
#else
  DPF(LDR "rmv:#NOT coded\n", ldr());
#endif
}

/**
 * @see mpscifo.h
 */
Msg_t *rmv_no_dbg_on_empty(MpscFifo_t *pQ) {
#if USE_ATOMIC_TYPES
  Msg_t* pTail = pQ->pTail;
  Msg_t* pNext = pTail->pNext;
  if ((pNext == NULL) && (pTail == &pQ->stub)) {
    // Q is "empty"
    return NULL;
  } else {
    return rmv(pQ);
  }
#else
  Msg_t* pTail = pQ->pTail;
  Msg_t* pNext = __atomic_load_n(&pTail->pNext, __ATOMIC_ACQUIRE);
  if ((pNext == NULL) && (pTail == &pQ->stub)) {
    // Q is "empty"
    return NULL;
  } else {
    return rmv(pQ);
  }
#endif
}

/**
 * @see mpscfifo.h
 */
void ret_msg(Msg_t* pMsg) {
  if ((pMsg != NULL) && (pMsg->pPool != NULL)) {
    ret(pMsg->pPool, pMsg);
  } else {
    if (pMsg == NULL) {
      DPF(LDR "ret:#No msg msg=%p\n", ldr(), pMsg);
    } else {
      DPF(LDR "ret:#No pool msg=%p pool=%p arg1=%lu arg2=%lu\n",
          ldr(), pMsg, pMsg->pPool, pMsg->arg1, pMsg->arg2);
    }
  }
}

/**
 * @see mpscfifo.h
 */
void send_rsp_or_ret(Msg_t* msg, uint64_t arg1) {
  if (msg->pRspQ != NULL) {
    MpscFifo_t* pRspQ = msg->pRspQ;
    msg->pRspQ = NULL;
    msg->arg1 = arg1;
    DPF(LDR "send_rsp_or_ret: send pRspQ=%p msg=%p pool=%p arg1=%lu arg2=%lu\n",
        ldr(), pRspQ, msg, msg->pPool, msg->arg1, msg->arg2);
    add(pRspQ, msg);
  } else {
    DPF(LDR "send_rsp_or_ret: no RspQ ret msg=%p pool=%p arg1=%lu arg2=%lu\n",
        ldr(), msg, msg->pPool, msg->arg1, msg->arg2);
    ret_msg(msg);
  }
}
