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

/**
 * @see mpscfifo.h
 */
MpscFifo_t *initMpscFifo(MpscFifo_t *pQ) {
  DPF("%ld  initMpscFifo:*pQ=%p\n", pthread_self(), pQ);
  pQ->pHead = &pQ->stub;
  pQ->pTail = &pQ->stub;
  pQ->stub.pNext = NULL;
  pQ->stub.pPool == NULL;
  return pQ;
}

/**
 * @see mpscfifo.h
 */
Msg_t *deinitMpscFifo(MpscFifo_t *pQ) {
  pQ->pHead = NULL;
  pQ->pTail = NULL;
}

/**
 * @see mpscifo.h
 */
void add(MpscFifo_t *pQ, Msg_t *pMsg) {
  DPF("%ld  add:+pQ=%p msg=%p arg1=%lu arg2=%lu\n",
        pthread_self(), pQ, pMsg, pMsg->arg1, pMsg->arg2);
#if USE_ATOMIC_TYPES
  pMsg->pNext = NULL;
  void** ptr_pHead = (void*)&pQ->pHead;
  Msg_t* pPrev = __atomic_exchange_n(ptr_pHead, pMsg, __ATOMIC_SEQ_CST);
  // rmv will stall spinning if preempted at this critical spot
  pPrev->pNext = pMsg;
  DPF("%ld  add:-pQ=%p msg=%p arg1=%lu arg2=%lu\n",
        pthread_self(), pQ, pMsg, pMsg->arg1, pMsg->arg2);
#else
  pMsg->pNext = NULL;
  Msg_t** ptr_pHead = &pQ->pHead;
  Msg_t* pPrev = __atomic_exchange_n(ptr_pHead, pMsg, __ATOMIC_ACQ_REL); //SEQ_CST);
  // rmv will stall spinning if preempted at this critical spot
  Msg_t** ptr_pNext = &pPrev->pNext;
  __atomic_store_n(ptr_pNext, pMsg, __ATOMIC_RELEASE); //SEQ_CST);
  DPF("%ld  add:-pQ=%p msg=%p arg1=%lu arg2=%lu\n",
        pthread_self(), pQ, pMsg, pMsg->arg1, pMsg->arg2);
#endif
}

/**
 * @see mpscifo.h
 */
Msg_t *rmv_non_stalling(MpscFifo_t *pQ) {
#if USE_ATOMIC_TYPES
  Msg_t* pTail = pQ->pTail;
  Msg_t* pNext = pTail->pNext;
  if (pTail == &pQ->stub) {
    // Nothing has been removed since Q was last empty
    if (pNext == NULL) {
      // Queue is empty
      DPF("%ld  rmv_non_stalling: is EMPTY pQ=%p msg=NULL\n", pthread_self(), pQ);
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
    DPF("%ld  rmv_non_stailling: got msg list has at least 1 more element pQ=%p msg=%p arg1=%lu arg2=%lu\n",
        pthread_self(), pQ, pTail, pTail->arg1, pTail->arg2);
    return pTail;
  }

  // We've reached the end of the list as known by pQ->pTail and
  // two conditions now exist either this is the last element
  // or the producer was preempted.
  Msg_t* pHead = pQ->pHead;
  if (pTail != pHead) {
    // First pHead != pTail then we know we were unlucky and a
    // producer was preempted. So we're not really empty but
    // since this is non-stalling we'll return NULL.
    DPF("%ld  rmv_non_stalling: preempted not really 'empty' pQ=%p msg=NULL\n", pthread_self(), pQ);
    return NULL;
  } else {
    // Second pHead == pTail then this is the last element
    // in which case we'll add stub to signify the Q if empty.
    // BUT now there are two more conditions.
    add(pQ, &pQ->stub);
    pNext = pTail->pNext;
    if (pNext != NULL) {
      pQ->pTail = pNext;
      DPF("%ld  rmv_non_stalling: return last element is EMPTY pQ=%p msg=NULL\n", pthread_self(), pQ);
      return pTail;
    } else {
      // While we added our node another thread also added a node
      // and the list isn't complete (?). Therefore we can't
      // remove this element until the other producer finishes
      // and since this is non-stalling return NULL.
      return NULL;
    }
  }
#else
  DPF("%ld  rmv_non_stailling: NOT coded\n", pthread_self());
#endif
}

/**
 * @see mpscifo.h
 */
Msg_t *rmv(MpscFifo_t *pQ) {
#if USE_ATOMIC_TYPES
  DPF("%ld  rmv: NOT coded\n", pthread_self());
#else
  DPF("%ld  rmv: NOT coded\n", pthread_self());
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
void ret(Msg_t* pMsg) {
  if ((pMsg != NULL) && (pMsg->pPool != NULL)) {
    DPF("%ld  ret: pool=%p msg=%p arg1=%lu arg2=%lu\n",
        pthread_self(), pMsg->pPool, pMsg, pMsg->arg1, pMsg->arg2);
    add(pMsg->pPool, pMsg);
  } else {
    if (pMsg == NULL) {
      DPF("%ld  ret: No msg msg=%p\n", pthread_self(), pMsg);
    } else {
      DPF("%ld  ret: No pool pool=%p msg=%p arg1=%lu arg2=%lu\n",
          pthread_self(), pMsg->pPool, pMsg, pMsg->arg1, pMsg->arg2);
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
    DPF("%ld  send_rsp_or_ret: send pRspQ=%p rsp arg1=%lu arg2=%lu\n",
        pthread_self(), pRspQ, msg->arg1, msg->arg2);
    add(pRspQ, msg);
  } else {
    DPF("%ld  send_rsp_or_ret: ret msg=%p arg1=%lu arg2=%lu\n",
        pthread_self(), msg, msg->arg1, msg->arg2);
    ret(msg);
  }
}
