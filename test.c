/**
 * This software is released into the public domain.
 */

#define NDEBUG

#define _DEFAULT_SOURCE
#define USE_RMV 0

#if USE_RMV
#define RMV rmv
#else
#define RMV rmv_non_stalling
#endif

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
#include <semaphore.h>

/**
 * We pass pointers in Msg_t.arg2 which is a uint64_t,
 * verify a void* fits.
 * TODO: sizeof(uint64_t) should be sizeof(Msg_t.arg2), how to do that?
 */
_Static_assert(sizeof(uint64_t) >= sizeof(void*), "Expect sizeof uint64_t >= sizeof void*");

const uint64_t ns_u64 = 1000000000ll;
const float ns_flt = 1000000000.0;

_Atomic(uint64_t) gTick = 0;

/**
 * Return the difference between to timespec in nano seconds
 */
uint64_t diff_timespec_ns(struct timespec* t1, struct timespec* t2) {
   uint64_t t1_ns = (t1->tv_sec * ns_u64) + t1->tv_nsec;
   uint64_t t2_ns = (t2->tv_sec * ns_u64) + t2->tv_nsec;
   return t1_ns - t2_ns;
}

typedef _Atomic(uint64_t) Counter;
//static typedef uint64_t Counter;

typedef struct MsgPool_t {
  Msg_t* msgs;
  uint32_t msg_count;
  MpscFifo_t fifo;
} MsgPool_t;

bool MsgPool_init(MsgPool_t* pool, uint32_t msg_count) {
  bool error;
  Msg_t* msgs;

  DPF(LDR "MsgPool_init:+pool=%p msg_count=%u\n",
      ldr(), pool, msg_count);

  // Allocate messages
  msgs = malloc(sizeof(Msg_t) * msg_count);
  if (msgs == NULL) {
    printf(LDR "MsgPool_init:-pool=%p ERROR unable to allocate messages, aborting msg_count=%u\n",
        ldr(), pool, msg_count);
    error = true;
    goto done;
  }

  // Initialize the fifo
  initMpscFifo(&pool->fifo);

  // Output info on the pool and messages
  DPF(LDR "MsgPool_init: pool=%p &msgs[0]=%p &msgs[1]=%p sizeof(Msg_t)=%lu(0x%lx)\n",
      ldr(), pool, &msgs[0], &msgs[1], sizeof(Msg_t), sizeof(Msg_t));

  // Create pool
  for (uint32_t i = 0; i < msg_count; i++) {
    Msg_t* msg = &msgs[i];
    msg->pPool = &pool->fifo;
    DPF(LDR "MsgPool_init: add %u msg=%p msg->pPool=%p\n", ldr(), i, msg, msg->pPool);
    add(&pool->fifo, msg);
  }

  DPF(LDR "MsgPool_init: pool=%p, pHead=%p, pTail=%p sizeof(*pool)=%lu(0x%lx)\n",
      ldr(), pool, pool->fifo.pHead, pool->fifo.pTail, sizeof(*pool), sizeof(*pool));

  error = false;
done:
  if (error) {
    free(msgs);
    pool->msgs = NULL;
    pool->msg_count = 0;
  } else {
    pool->msgs = msgs;
    pool->msg_count = msg_count;
  }

  DPF(LDR "MsgPool_init:-pool=%p msg_count=%u error=%u\n",
      ldr(), pool, msg_count, error);

  return error;
}

void MsgPool_deinit(MsgPool_t* pool) {
  DPF(LDR "MsgPool_deinit:+pool=%p msgs=%p\n", ldr(), pool, pool->msgs);
  if (pool->msgs != NULL) {
    // Empty the pool
    DPF(LDR "MsgPool_deinit: pool=%p pool->msg_count=%u\n", ldr(), pool, pool->msg_count);
    for (uint32_t i = 0; i < pool->msg_count; i++) {
      Msg_t *msg;

      // Wait until this is returned
      // TODO: Bug it may never be returned!
      bool once = false;
      while ((msg = RMV(&pool->fifo)) == NULL) {
        if (!once) {
          once = true;
          printf(LDR "MsgPool_deinit: waiting for %u\n", ldr(), i);
        }
        sched_yield();
      }

      DPF(LDR "MsgPool_deinit: removed %u msg=%p\n", ldr(), i, msg);
    }

    DPF(LDR "MsgPool_deinit: pool=%p deinitMpscFifo fifo=%p\n", ldr(), pool, &pool->fifo);
    deinitMpscFifo(&pool->fifo);

    DPF(LDR "MsgPool_deinit: pool=%p free msgs=%p\n", ldr(), pool, pool->msgs);
    free(pool->msgs);
    pool->msgs = NULL;
    pool->msg_count = 0;
  }
  DPF(LDR "MsgPool_deinit:-pool=%p\n", ldr(), pool);
}

Msg_t* MsgPool_get_msg(MsgPool_t* pool) {
  DPF(LDR "MsgPool_get_msg:+pool=%p\n", ldr(), pool);
  Msg_t* msg = RMV(&pool->fifo);
  if (msg != NULL) {
    msg->pRspQ = NULL;
    msg->arg1 = 0;
    msg->arg2 = 0;
    DPF(LDR "MsgPool_get_msg: pool=%p got msg=%p pool=%p\n", ldr(), pool, msg, msg->pPool);
  }
  DPF(LDR "MsgPool_get_msg:-pool=%p msg=%p\n", ldr(), pool, msg);
  return msg;
}

typedef struct ClientParams ClientParams;

typedef struct ClientParams {
  MpscFifo_t cmdFifo;

  pthread_t thread;
  uint32_t msg_count;
  uint32_t max_peer_count;

  ClientParams** peers;
  uint32_t peer_send_idx;
  uint32_t peers_connected;


  MsgPool_t pool;

  uint64_t error_count;
  uint64_t msgs_processed;
  sem_t sem_ready;
  sem_t sem_waiting;
} ClientParams;

#define CmdUnknown       0 // arg2 == the command that's unknown
#define CmdDoNothing     1
#define CmdDidNothing    2
#define CmdConnect       3 // arg2 == MpscFifo_t* to connect with
#define CmdConnected     4 // arg2 == MpscFifo_t* connected to
#define CmdDisconnectAll 5
#define CmdDisconnected  6
#define CmdStop          7
#define CmdStopped       8
#define CmdSendToPeers   9
#define CmdSent          10

/**
 * Send messages CmdDoNothing to all of the peers
 */
void send_to_peers(ClientParams* cp) {
  DPF(LDR "send_to_peers:+param=%p\n", ldr(), cp);

  for (uint32_t i = 0; i < cp->peers_connected; i++) {
    Msg_t* msg = MsgPool_get_msg(&cp->pool);
    if (msg == NULL) {
      DPF(LDR "send_to_peers: param=%p whoops no more messages, sent to %u peers\n",
          ldr(), cp, i);
      return;
    }
    ClientParams* peer = cp->peers[cp->peer_send_idx];
    msg->arg1 = CmdDoNothing;
    DPF(LDR "send_to_peers: param=%p send to peer=%p msg=%p msg->arg1=%lu CmdDoNothing\n",
       ldr(), cp, peer, msg, msg->arg1);
    add(&peer->cmdFifo, msg);
    sem_post(&peer->sem_waiting);
    DPF(LDR "send_to_peers: param=%p SENT to peer=%p msg=%p msg->arg1=%lu CmdDoNothing\n",
       ldr(), cp, peer, msg, msg->arg1);
    cp->peer_send_idx += 1;
    if (cp->peer_send_idx >= cp->peers_connected) {
      cp->peer_send_idx = 0;
    }
  }
  DPF(LDR "send_to_peers:-param=%p\n", ldr(), cp);
}

static void* client(void* p) {
  DPF(LDR "client:+param=%p\n", ldr(), p);
  Msg_t* msg;

  ClientParams* cp = (ClientParams*)p;

  cp->error_count = 0;
  cp->msgs_processed = 0;

  if (cp->max_peer_count > 0) {
    DPF(LDR "client: param=%p allocate peers max_peer_count=%u\n",
        ldr(), p, cp->max_peer_count);
    cp->peers = malloc(sizeof(ClientParams*) * cp->max_peer_count);
    if (cp->peers == NULL) {
      printf(LDR "client: param=%p ERROR unable to allocate peers max_peer_count=%u\n",
          ldr(), p, cp->max_peer_count);
      cp->error_count += 1;
    }
  } else {
    DPF(LDR "client: param=%p No peers max_peer_count=%d\n",
        ldr(), p, cp->max_peer_count);
    cp->peers = NULL;
  }
  cp->peers_connected = 0;
  cp->peer_send_idx = 0;


  // Init local msg pool
  DPF(LDR "client: init msg pool=%p\n", ldr(), &cp->pool);
  bool error = MsgPool_init(&cp->pool, cp->msg_count);
  if (error) {
    printf(LDR "client: param=%p ERROR unable to create msgs for pool\n", ldr(), p);
    cp->error_count += 1;
  }

  // Init cmdFifo
  initMpscFifo(&cp->cmdFifo);

  DPF(LDR "client: param=%p cp->cmdFifo=%p\n", ldr(), p, &cp->cmdFifo);


  // Signal we're ready
  sem_post(&cp->sem_ready);

  // While we're not done wait for a signal to do work
  // do the work and signal work is complete.
  while (true) {
    DPF(LDR "client: param=%p waiting\n", ldr(), p);
#if USE_RMV == 1
    sem_wait(&cp->sem_waiting);
    while((msg = rmv(&cp->cmdFifo)) != NULL) {
#else
    sched_yield();
    while((msg = rmv_non_stalling(&cp->cmdFifo)) != NULL) {
#endif
      cp->msgs_processed += 1;
      if (msg != NULL) {
        DPF(LDR "client:^param=%p msg=%p arg1=%lu msgs_processed=%lu\n",
            ldr(), p, msg, msg->arg1, cp->msgs_processed);
        switch (msg->arg1) {
          case CmdDoNothing: {
            DPF(LDR "client:+param=%p msg=%p CmdDoNothing\n", ldr(), p, msg);
            send_rsp_or_ret(msg, CmdDidNothing);
            DPF(LDR "client:-param=%p msg=%p CmdDoNothing\n", ldr(), p, msg);
            break;
          }
          case CmdStop: {
            DPF(LDR "client: param=%p msg=%p CmdStop\n", ldr(), p, msg);
            send_rsp_or_ret(msg, CmdStopped);
            DPF(LDR "client:-param=%p msg=%p CmdStop\n", ldr(), p, msg);
            goto done;
          }
          case CmdConnect: {
            DPF(LDR "client:+param=%p msg=%p CmdConnect peers_connected=%u max_peer_count=%u\n",
                ldr(), p, msg, cp->peers_connected, cp->max_peer_count);
            if (cp->peers != NULL) {
              if (cp->peers_connected < cp->max_peer_count) {
                cp->peers[cp->peers_connected] = (ClientParams*)msg->arg2;
                DPF(LDR "client: param=%p CmdConnect to peer=%p\n",
                    ldr(), p, cp->peers[cp->peers_connected]);
                cp->peers_connected += 1;
              } else {
                printf(LDR "client: param=%p CmdConnect ERROR msg->arg2=%lx to many peers "
                    "peers_connected=%u >= cp->max_peer_count=%u\n",
                    ldr(), p, msg->arg2, cp->peers_connected, cp->max_peer_count);
              }
            }
            DPF(LDR "client: param=%p msg=%p CmdConnect call send_rsp_or_ret\n", ldr(), p, msg);
            send_rsp_or_ret(msg, CmdConnected);
            DPF(LDR "client:-param=%p msg=%p CmdConnect peers_connected=%u max_peer_count=%u\n",
                ldr(), p, msg, cp->peers_connected, cp->max_peer_count);
            break;
          }
          case CmdDisconnectAll: {
            DPF(LDR "client:+param=%p msg=%p CmdDisconnectAll peers_connected=%u max_peer_count=%u\n",
                ldr(), p, msg, cp->peers_connected, cp->max_peer_count);
            if (cp->peers != NULL) {
              cp->peers_connected = 0;
            }
            send_rsp_or_ret(msg, CmdDisconnected);
            DPF(LDR "client:-param=%p msg=%p CmdDisconnectAll peers_connected=%u max_peer_count=%u\n",
                ldr(), p, msg, cp->peers_connected, cp->max_peer_count);
            break;
          }
          case CmdSendToPeers: {
            DPF(LDR "client:+param=%p msg=%p CmdSendToPeers\n", ldr(), p, msg);
            send_rsp_or_ret(msg, CmdSent);
            send_to_peers(cp);
            DPF(LDR "client:-param=%p msg=%p CmdSendToPeers\n", ldr(), p, msg);
            break;
          }
          default: {
            DPF(LDR "client:+param=%p ERROR msg=%p Uknown arg1=%lu\n",
                ldr(), p, msg, msg->arg1);
            cp->error_count += 1;
            msg->arg2 = msg->arg1;
            send_rsp_or_ret(msg, CmdUnknown);
            ret_msg(msg);
            DPF(LDR "client:-param=%p ERROR msg=%p Uknown arg1=%lu\n",
                ldr(), p, msg, msg->arg1);
            break;
          }
        }
      } else {
        cp->error_count += 1;
        DPF(LDR "client: param=%p ERROR msg=NULL\n", ldr(), p);
      }
    }
  }

done:
  // Flush any messages in the cmdFifo
  DPF(LDR "client: param=%p done, flushing fifo\n", ldr(), p);
  uint32_t unprocessed = 0;
  while ((msg = RMV(&cp->cmdFifo)) != NULL) {
    printf(LDR "client: param=%p ret msg=%p\n", ldr(), p, msg);
    unprocessed += 1;
    ret_msg(msg);
  }

  // deinit cmd fifo
  DPF(LDR "client: param=%p deinit cmdFifo=%p\n", ldr(), p, &cp->cmdFifo);
  deinitMpscFifo(&cp->cmdFifo);

  // deinit msg pool
  DPF(LDR "client: param=%p deinit msg pool=%p\n", ldr(), p, &cp->pool);
  MsgPool_deinit(&cp->pool);

  DPF(LDR "client:-param=%p error_count=%lu\n", ldr(), p, cp->error_count);
  return NULL;
}

// Return 0 if successful !0 if an error
uint32_t wait_for_rsp(MpscFifo_t* fifo, uint64_t rsp_expected, void* client, uint32_t client_idx) {
  uint32_t retv;
  Msg_t* msg;
  DPF(LDR "wait_for_rsp:+fifo=%p rsp_expected %lu client[%u]=%p\n",
      ldr(), fifo, rsp_expected, client_idx, client);

  // TODO: Add MpscFifo_t.sem_waiting??
  bool once = false;
  while ((msg = RMV(fifo)) == NULL) {
    if (!once) {
      once = true;
      DPF(LDR "wait_for_rsp: fifo=%p waiting for arg1=%lu client[%u]=%p\n",
          ldr(), fifo, rsp_expected, client_idx, client);
    }
    sched_yield();
  }
  if (msg->arg1 != rsp_expected) {
    DPF(LDR "wait_for_rsp: fifo=%p ERROR unexpected arg1=%lu expected %lu arg2=%lu, client[%u]=%p\n",
        ldr(), fifo, msg->arg1, rsp_expected, msg->arg2, client_idx, client);
    retv = 1;
  } else {
    DPF(LDR "wait_for_rsp: fifo=%p got ar1=%lu arg2=%lu client[%u]=%p\n",
        ldr(), fifo, rsp_expected, msg->arg2, client_idx, client);
    retv = 0;
  }
  ret_msg(msg);
  DPF(LDR "wait_for_rsp:-fifo=%p rsp_expected %lu client[%u]=%p retv=%u\n",
      ldr(), fifo, rsp_expected, client_idx, client, retv);
  return retv;
}

bool multi_thread_main(const uint32_t client_count, const uint64_t loops,
    const uint32_t msg_count) {
  bool error;
  MpscFifo_t cmdFifo;
  ClientParams* clients;
  MsgPool_t pool;
  uint32_t clients_created = 0;
  uint64_t msgs_sent = 0;
  uint64_t no_msgs_count = 0;

  struct timespec time_start;
  struct timespec time_looping;
  struct timespec time_done;
  struct timespec time_disconnected;
  struct timespec time_stopped;
  struct timespec time_complete;

  printf(LDR "multi_thread_msg:+client_count=%u loops=%lu msg_count=%u\n",
      ldr(), client_count, loops, msg_count);

  clock_gettime(CLOCK_REALTIME, &time_start);

  if (client_count == 0) {
    printf(LDR "multi_thread_msg: ERROR client_count=%d, aborting\n",
        ldr(), client_count);
    error = true;
    goto done;
  }

  clients = malloc(sizeof(ClientParams) * client_count);
  if (clients == NULL) {
    printf(LDR "multi_thread_msg: ERROR Unable to allocate clients array, aborting\n", ldr());
    error = true;
    goto done;
  }

  DPF(LDR "multi_thread_msg: init msg pool=%p\n", ldr(), &pool);
  error = MsgPool_init(&pool, msg_count);
  if (error) {
    printf(LDR "multi_thread_msg: ERROR Unable to allocate messages, aborting\n", ldr());
    goto done;
  }

  initMpscFifo(&cmdFifo);
  DPF(LDR "multi_thread_msg: cmdFifo=%p\n", ldr(), &cmdFifo);

  // Create the clients
  for (uint32_t i = 0; i < client_count; i++, clients_created++) {
    ClientParams* param = &clients[i];
    param->msg_count = msg_count;
    param->max_peer_count = client_count;

    sem_init(&param->sem_ready, 0, 0);
    sem_init(&param->sem_waiting, 0, 0);

    int retv = pthread_create(&param->thread, NULL, client, (void*)&clients[i]);
    if (retv != 0) {
      printf(LDR "multi_thread_msg: ERROR thread creation , clients[%u]=%p retv=%d\n",
          ldr(), i, param, retv);
      error = true;
      goto done;
    }

    // Wait until it starts
    sem_wait(&param->sem_ready);
  }
  DPF(LDR "multi_thread_msg: created %u clients\n", ldr(), clients_created);


  // Connect every client to every other client except themselves
  for (uint32_t i = 0; i < clients_created; i++) {
    ClientParams* client = &clients[i];
    for (uint32_t peer_idx = 0; peer_idx < clients_created; peer_idx++) {
      ClientParams* peer = &clients[peer_idx];
      if (peer_idx != i) {
        Msg_t* msg = MsgPool_get_msg(&pool);
        if (msg != NULL) {
          msg->pRspQ = &cmdFifo;
          msg->arg1 = CmdConnect;
          msg->arg2 = (uint64_t)peer;
          DPF(LDR "multi_thread_msg: send client=%p msg=%p arg1=%lu CmdConnect\n",
              ldr(), client, msg, msg->arg1);
          add(&client->cmdFifo, msg);
          sem_post(&client->sem_waiting);
        }
        if (wait_for_rsp(&cmdFifo, CmdConnected, client, i)) {
          error = true;
          goto done;
        }
      }
    }
  }

  DPF(LDR "multi_thread_msg: send CmdSendToPeers to %u clients\n", ldr(), clients_created);

  clock_gettime(CLOCK_REALTIME, &time_looping);

  // Loop though all the clients asking them to send to their peers
  for (uint32_t i = 0; i < loops; i++) {
    for (uint32_t c = 0; c < clients_created; c++) {
      // Test both flavors of rmv
      Msg_t* msg;
      msg = RMV(&pool.fifo);

      if (msg != NULL) {
        ClientParams* client = &clients[c];
        msg->arg1 = CmdSendToPeers;
        DPF(LDR "multi_thread_msg: send client=%p msg=%p arg1=%lu CmdSendToPeers\n",
            ldr(), client, msg, msg->arg1);
        add(&client->cmdFifo, msg);
        sem_post(&client->sem_waiting);
        msgs_sent += 1;
      } else {
        no_msgs_count += 1;
        DPF(LDR "multi_thread_msg: Whoops msg == NULL c=%u msgs_sent=%lu no_msgs_count=%lu\n",
            ldr(), c, msgs_sent, no_msgs_count);
        sched_yield();
      }
    }
  }

  error = false;

done:
  clock_gettime(CLOCK_REALTIME, &time_done);

  DPF(LDR "multi_thread_msg: done, send CmdDisconnectAll %u clients\n",
      ldr(), clients_created);
  uint64_t msgs_processed = 0;
  for (uint32_t i = 0; i < clients_created; i++) {
    ClientParams* client = &clients[i];

    // Request the client to stop
    Msg_t* msg = MsgPool_get_msg(&pool);
    bool once = false;
    while (msg == NULL) {
      if (!once) {
        once = true;
        DPF(LDR "multi_thread_msg: waiting for msg to send %u CmdDisconnectAll\n",
            ldr(), i);
      }
      sched_yield();
      msg = MsgPool_get_msg(&pool);
    }
    msg->pRspQ = &cmdFifo;
    msg->arg1 = CmdDisconnectAll;
    DPF(LDR "multi_thread_msg: send %u client=%p msg=%p msg->arg1=%lu CmdDisconnectAll\n",
        ldr(), i, client, msg, msg->arg1);
    add(&client->cmdFifo, msg);
    sem_post(&client->sem_waiting);
    if (wait_for_rsp(&cmdFifo, CmdDisconnected, client, i)) {
      error = true;
      goto done;
    }
  }

  clock_gettime(CLOCK_REALTIME, &time_disconnected);

  DPF(LDR "multi_thread_msg: done, send CmdStop %u clients\n",
      ldr(), clients_created);
  for (uint32_t i = 0; i < clients_created; i++) {
    ClientParams* client = &clients[i];

    // Request the client to stop
    Msg_t* msg = MsgPool_get_msg(&pool);
    bool once = false;
    while (msg == NULL) {
      if (!once) {
        once = true;
        DPF(LDR "multi_thread_msg: waiting for msg to send %u CmdStop\n", ldr(), i);
      }
      sched_yield();
      msg = MsgPool_get_msg(&pool);
    }
    DPF(LDR "multi_thread_msg: send %u CmdStop\n", ldr(), i);
    msg->pRspQ = &cmdFifo;
    msg->arg1 = CmdStop;
    DPF(LDR "multi_thread_msg: send client=%p msg=%p msg->arg1=%lu CmdStop\n", ldr(),
       client, msg, msg->arg1);
    add(&client->cmdFifo, msg);
    sem_post(&client->sem_waiting);
    if (wait_for_rsp(&cmdFifo, CmdStopped, client, i)) {
      error = true;
      goto done;
    }
  }

  clock_gettime(CLOCK_REALTIME, &time_stopped);

  DPF(LDR "multi_thread_msg: done, joining %u clients\n", ldr(), clients_created);
  for (uint32_t i = 0; i < clients_created; i++) {
    ClientParams* client = &clients[i];
    // Wait until the thread completes
    int retv = pthread_join(client->thread, NULL);
    if (retv != 0) {
      printf(LDR "multi_thread_msg: ERROR joining failed, clients[%u]=%p retv=%d\n",
          ldr(), i, (void*)client, retv);
    }

    // Cleanup resources
    sem_destroy(&client->sem_ready);
    sem_destroy(&client->sem_waiting);

    // Record if clients discovered any errors
    if (client->error_count != 0) {
      printf(LDR "multi_thread_msg: ERROR clients[%u]=%p error_count=%lu\n",
          ldr(), i, (void*)client, client->error_count);
      error = true;
    }
    msgs_processed += client->msgs_processed;
    DPF(LDR "multi_thread_msg: clients[%u]=%p msgs_processed=%lu error_count=%lu\n",
        ldr(), i, (void*)client, client->msgs_processed, client->error_count);
  }

  // Deinit the cmdFifo
  DPF(LDR "multi_thread_msg: deinit cmdFifo=%p\n", ldr(), &cmdFifo);
  deinitMpscFifo(&cmdFifo);

  // Deinit the msg pool
  DPF(LDR "multi_thread_msg: deinit msg pool=%p\n", ldr(), &pool);
  MsgPool_deinit(&pool);

  clock_gettime(CLOCK_REALTIME, &time_complete);

  uint64_t expected_value = loops * clients_created;
  uint64_t sum = msgs_sent + no_msgs_count;
  if (sum != expected_value) {
    printf(LDR "multi_thread_msg: ERROR sum=%lu != expected_value=%lu\n",
       ldr(), sum, expected_value);
    error = true;
  }

  printf(LDR "multi_thread_msg: msgs_processed=%lu msgs_sent=%lu "
      "no_msgs_count=%lu\n", ldr(), msgs_processed, msgs_sent, no_msgs_count);

  DPF(LDR "time_start=%lu.%lu\n", ldr(), time_start.tv_sec, time_start.tv_nsec);
  DPF(LDR "time_looping=%lu.%lu\n", ldr(), time_looping.tv_sec, time_looping.tv_nsec);
  DPF(LDR "time_done=%lu.%lu\n", ldr(), time_done.tv_sec, time_done.tv_nsec);
  DPF(LDR "time_disconnected=%lu.%lu\n", ldr(), time_disconnected.tv_sec, time_disconnected.tv_nsec);
  DPF(LDR "time_stopped=%lu.%lu\n", ldr(), time_stopped.tv_sec, time_stopped.tv_nsec);
  DPF(LDR "time_complete=%lu.%lu\n", ldr(), time_complete.tv_sec, time_complete.tv_nsec);

  printf(LDR "startup=%.6f\n", ldr(), diff_timespec_ns(&time_looping, &time_start) / ns_flt);
  printf(LDR "looping=%.6f\n", ldr(), diff_timespec_ns(&time_done, &time_looping) / ns_flt);
  printf(LDR "disconneting=%.6f\n", ldr(), diff_timespec_ns(&time_disconnected, &time_done) / ns_flt);
  printf(LDR "stopping=%.6f\n", ldr(), diff_timespec_ns(&time_stopped, &time_disconnected) / ns_flt);
  printf(LDR "complete=%.6f\n", ldr(), diff_timespec_ns(&time_complete, &time_stopped) / ns_flt);

  uint64_t processing_ns = diff_timespec_ns(&time_complete, &time_looping);
  printf(LDR "processing=%.3fs\n", ldr(), processing_ns / ns_flt);
  uint64_t msgs_per_sec = (msgs_processed * ns_u64) / processing_ns;
  printf(LDR "msgs_per_sec=%lu\n", ldr(), msgs_per_sec);
  float ns_per_msg = (float)processing_ns / (float)msgs_processed;
  printf(LDR "ns_per_msg=%.1fns\n", ldr(), ns_per_msg);
  printf(LDR "total=%.3f\n", ldr(), diff_timespec_ns(&time_complete, &time_start) / ns_flt);

  printf(LDR "multi_thread_msg:-error=%u\n\n", ldr(), error);

  return error;
}

int main(int argc, char *argv[]) {
  bool error = false;

  if (argc != 4) {
    printf("Usage:\n");
    printf(" %s client_count loops msg_count\n", argv[0]);
    return 1;
  }

  u_int32_t client_count = strtoul(argv[1], NULL, 10);
  u_int64_t loops = strtoull(argv[2], NULL, 10);
  u_int32_t msg_count = strtoul(argv[3], NULL, 10);

  printf("test client_count=%u loops=%lu msg_count=%u\n", client_count, loops, msg_count);

  error |= multi_thread_main(client_count, loops, msg_count);

  if (!error) {
    printf("Success\n");
  }

  return error ? 1 : 0;
}
