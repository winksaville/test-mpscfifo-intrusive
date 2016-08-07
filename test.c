/**
 * This software is released into the public domain.
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
#include <semaphore.h>

#define tid() pthread_self()

#define CRASH() do { *((volatile uint8_t*)0) = 0; } while(0)

/**
 * We pass pointers in Msg_t.arg2 which is a uint64_t,
 * verify a void* fits.
 * TODO: sizeof(uint64_t) should be sizeof(Msg_t.arg2), how to do that?
 */
_Static_assert(sizeof(uint64_t) >= sizeof(void*), "Expect sizeof uint64_t >= sizeof void*");

const uint64_t ns_u64 = 1000000000ll;
const float ns_flt = 1000000000.0;

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

  DPF("%ld  MsgPool_init:+pool=%p msg_count=%u\n",
      tid(), pool, msg_count);

  // Allocate messages
  msgs = malloc(sizeof(Msg_t) * msg_count);
  if (msgs == NULL) {
    printf("%ld  MsgPool_init:-pool=%p ERROR unable to allocate messages, aborting msg_count=%u\n",
        tid(), pool, msg_count);
    error = true;
    goto done;
  }

  // Initialize the fifo
  initMpscFifo(&pool->fifo);

  // Output info on the pool and messages
  DPF("%ld  MsgPool_init: pool=%p &msgs[0]=%p &msgs[1]=%p sizeof(Msg_t)=%lu(0x%lx)\n",
      tid(), pool, &msgs[0], &msgs[1], sizeof(Msg_t), sizeof(Msg_t));

  // Create pool
  for (uint32_t i = 0; i < msg_count; i++) {
    Msg_t* msg = &msgs[i];
    msg->pPool = &pool->fifo;
    DPF("%ld  MsgPool_init: add %u msg=%p msg->pPool=%p\n", tid(), i, msg, msg->pPool);
    add(&pool->fifo, msg);
  }

  DPF("%ld  MsgPool_init: pool=%p, pHead=%p, pTail=%p sizeof(*pool)=%lu(0x%lx)\n",
      tid(), pool, pool->fifo.pHead, pool->fifo.pTail, sizeof(*pool), sizeof(*pool));

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

  DPF("%ld  MsgPool_init:-pool=%p msg_count=%u error=%u\n",
      tid(), pool, msg_count, error);

  return error;
}

void MsgPool_deinit(MsgPool_t* pool) {
  DPF("%ld  MsgPool_deinit:+pool=%p msgs=%p\n", tid(), pool, pool->msgs);
  if (pool->msgs != NULL) {
    // Empty the pool
    for (uint32_t i = 0; i < pool->msg_count; i++) {
      Msg_t *msg;

      // Wait until this is returned
      // TODO: Bug it may never be returned!
      bool once = false;
      while ((msg = rmv(&pool->fifo)) == NULL) {
        if (!once) {
          once = true;
          printf("%ld  MsgPool_deinit: waiting for %u\n", tid(), i);
        }
        sched_yield();
      }

      DPF("%ld  MsgPool_deinit: removed %u msg=%p\n", tid(), i, msg);
    }

    DPF("%ld  MsgPool_deinit: pool=%p deinitMpscFifo fifo=%p\n", tid(), pool, &pool->fifo);
    deinitMpscFifo(&pool->fifo);

    DPF("%ld  MsgPool_deinit: pool=%p free msgs=%p\n", tid(), pool, pool->msgs);
    free(pool->msgs);
    pool->msgs = NULL;
    pool->msg_count = 0;
  }
  DPF("%ld  MsgPool_deinit:-pool=%p\n", tid(), pool);
}

Msg_t* MsgPool_get_msg(MsgPool_t* pool) {
  DPF("%ld  MsgPool_get_msg:+pool=%p\n", tid(), pool);
  Msg_t* msg = rmv(&pool->fifo);
  if (msg != NULL) {
    msg->pRspQ = NULL;
    msg->arg1 = 0;
    msg->arg2 = 0;
    DPF("%ld  MsgPool_get_msg: pool=%p got msg=%p pool=%p\n", tid(), pool, msg, msg->pPool);
  }
  DPF("%ld  MsgPool_get_msg:+pool=%p msg=%p\n", tid(), pool, msg);
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
  DPF("%ld  send_to_peers:+param=%p\n", tid(), cp);

  for (uint32_t i = 0; i < cp->peers_connected; i++) {
    Msg_t* msg = MsgPool_get_msg(&cp->pool);
    if (msg == NULL) {
      DPF("%ld  send_to_peers: param=%p whoops no more messages, sent to %u peers\n",
          tid(), cp, i);
      return;
    }
    ClientParams* peer = cp->peers[cp->peer_send_idx];
    msg->arg1 = CmdDoNothing;
    DPF("%ld  send_to_peers: param=%p send to peer=%p msg=%p msg->arg1=%lu CmdDoNothing\n",
       tid(), cp, peer, msg, msg->arg1);
    add(&peer->cmdFifo, msg);
    sem_post(&peer->sem_waiting);
    DPF("%ld  send_to_peers: param=%p SENT to peer=%p msg=%p msg->arg1=%lu CmdDoNothing\n",
       tid(), cp, peer, msg, msg->arg1);
    cp->peer_send_idx += 1;
    if (cp->peer_send_idx >= cp->peers_connected) {
      cp->peer_send_idx = 0;
    }
  }
  DPF("%ld  send_to_peers:-param=%p\n", tid(), cp);
}

static void* client(void* p) {
  DPF("%ld  client:+param=%p\n", tid(), p);
  Msg_t* msg;

  ClientParams* cp = (ClientParams*)p;

  cp->error_count = 0;
  cp->msgs_processed = 0;

  if (cp->max_peer_count > 0) {
    DPF("%ld  client: param=%p allocate peers max_peer_count=%u\n",
        tid(), p, cp->max_peer_count);
    cp->peers = malloc(sizeof(ClientParams*) * cp->max_peer_count);
    if (cp->peers == NULL) {
      printf("%ld  client: param=%p ERROR unable to allocate peers max_peer_count=%u\n",
          tid(), p, cp->max_peer_count);
      cp->error_count += 1;
    }
  } else {
    DPF("%ld  client: param=%p No peers max_peer_count=%d\n",
        tid(), p, cp->max_peer_count);
    cp->peers = NULL;
  }
  cp->peers_connected = 0;
  cp->peer_send_idx = 0;


  // Init local msg pool
  DPF("%ld  client: init msg pool=%p\n", tid(), &cp->pool);
  bool error = MsgPool_init(&cp->pool, cp->msg_count);
  if (error) {
    printf("%ld  client: param=%p ERROR unable to create msgs for pool\n", tid(), p);
    cp->error_count += 1;
  }

  // Init cmdFifo
  initMpscFifo(&cp->cmdFifo);

  DPF("%ld  client: param=%p cp->cmdFifo=%p\n", tid(), p, &cp->cmdFifo);


  // Signal we're ready
  sem_post(&cp->sem_ready);

  // While we're not done wait for a signal to do work
  // do the work and signal work is complete.
  while (true) {
    DPF("%ld  client: param=%p waiting\n", tid(), p);
    sem_wait(&cp->sem_waiting);

    while((msg = rmv(&cp->cmdFifo)) != NULL) {
      cp->msgs_processed += 1;
      if (msg != NULL) {
        DPF("%ld  client:^param=%p msg=%p arg1=%lu msgs_processed=%lu\n",
            tid(), p, msg, msg->arg1, cp->msgs_processed);
        switch (msg->arg1) {
          case CmdDoNothing: {
            DPF("%ld  client:+param=%p msg=%p CmdDoNothing\n", tid(), p, msg);
            send_rsp_or_ret(msg, CmdDidNothing);
            DPF("%ld  client:-param=%p msg=%p CmdDoNothing\n", tid(), p, msg);
            break;
          }
          case CmdStop: {
            DPF("%ld  client: param=%p msg=%p CmdStop\n", tid(), p, msg);
            send_rsp_or_ret(msg, CmdStopped);
            DPF("%ld  client:-param=%p msg=%p CmdStop\n", tid(), p, msg);
            goto done;
          }
          case CmdConnect: {
            DPF("%ld  client:+param=%p msg=%p CmdConnect peers_connected=%u max_peer_count=%u\n",
                tid(), p, msg, cp->peers_connected, cp->max_peer_count);
            if (cp->peers != NULL) {
              if (cp->peers_connected < cp->max_peer_count) {
                cp->peers[cp->peers_connected] = (ClientParams*)msg->arg2;
                DPF("%ld  client: param=%p CmdConnect to peer=%p\n",
                    tid(), p, cp->peers[cp->peers_connected]);
                cp->peers_connected += 1;
              } else {
                printf("%ld  client: param=%p CmdConnect ERROR msg->arg2=%lx to many peers "
                    "peers_connected=%u >= cp->max_peer_count=%u\n",
                    tid(), p, msg->arg2, cp->peers_connected, cp->max_peer_count);
              }
            }
            DPF("%ld  client: param=%p msg=%p CmdConnect call send_rsp_or_ret\n", tid(), p, msg);
            send_rsp_or_ret(msg, CmdConnected);
            DPF("%ld  client:-param=%p msg=%p CmdConnect peers_connected=%u max_peer_count=%u\n",
                tid(), p, msg, cp->peers_connected, cp->max_peer_count);
            break;
          }
          case CmdDisconnectAll: {
            DPF("%ld  client:+param=%p msg=%p CmdDisconnectAll peers_connected=%u max_peer_count=%u\n",
                tid(), p, msg, cp->peers_connected, cp->max_peer_count);
            if (cp->peers != NULL) {
              cp->peers_connected = 0;
            }
            send_rsp_or_ret(msg, CmdDisconnected);
            DPF("%ld  client:-param=%p msg=%p CmdDisconnectAll peers_connected=%u max_peer_count=%u\n",
                tid(), p, msg, cp->peers_connected, cp->max_peer_count);
            break;
          }
          case CmdSendToPeers: {
            DPF("%ld  client:+param=%p msg=%p CmdSendToPeers\n", tid(), p, msg);
            send_rsp_or_ret(msg, CmdSent);
            send_to_peers(cp);
            DPF("%ld  client:-param=%p msg=%p CmdSendToPeers\n", tid(), p, msg);
            break;
          }
          default: {
            DPF("%ld  client:+param=%p ERROR msg=%p Uknown arg1=%lu\n",
                tid(), p, msg, msg->arg1);
            cp->error_count += 1;
            msg->arg2 = msg->arg1;
            send_rsp_or_ret(msg, CmdUnknown);
            ret(msg);
            DPF("%ld  client:-param=%p ERROR msg=%p Uknown arg1=%lu\n",
                tid(), p, msg, msg->arg1);
            break;
          }
        }
      } else {
        cp->error_count += 1;
        DPF("%ld  client: param=%p ERROR msg=NULL\n", tid(), p);
      }
    }
  }

done:
  // Flush any messages in the cmdFifo
  DPF("%ld  client: param=%p done, flushing fifo\n", tid(), p);
  uint32_t unprocessed = 0;
  while ((msg = rmv(&cp->cmdFifo)) != NULL) {
    printf("%ld  client: param=%p ret msg=%p\n", tid(), p, msg);
    unprocessed += 1;
    ret(msg);
  }

  // deinit cmd fifo
  DPF("%ld  client: param=%p deinit cmdFifo=%p\n", tid(), p, &cp->cmdFifo);
  deinitMpscFifo(&cp->cmdFifo);

  // deinit msg pool
  DPF("%ld  client: param=%p deinit msg pool=%p\n", tid(), p, &cp->pool);
  MsgPool_deinit(&cp->pool);

  DPF("%ld  client:-param=%p error_count=%lu\n", tid(), p, cp->error_count);
  return NULL;
}

// Return 0 if successful !0 if an error
uint32_t wait_for_rsp(MpscFifo_t* fifo, uint64_t rsp_expected, void* client, uint32_t client_idx) {
  uint32_t retv;
  Msg_t* msg;
  DPF("%ld  wait_for_rsp:+fifo=%p rsp_expected %lu client[%u]=%p\n",
      tid(), fifo, rsp_expected, client_idx, client);

  // TODO: Add MpscFifo_t.sem_waiting??
  bool once = false;
  while ((msg = rmv_no_dbg_on_empty(fifo)) == NULL) {
    if (!once) {
      once = true;
      DPF("%ld  wait_for_rsp: fifo=%p waiting for arg1=%lu client[%u]=%p\n",
          tid(), fifo, rsp_expected, client_idx, client);
    }
    sched_yield();
  }
  if (msg->arg1 != rsp_expected) {
    DPF("%ld  wait_for_rsp: fifo=%p ERROR unexpected arg1=%lu expected %lu arg2=%lu, client[%u]=%p\n",
        tid(), fifo, msg->arg1, rsp_expected, msg->arg2, client_idx, client);
    retv = 1;
  } else {
    DPF("%ld  wait_for_rsp: fifo=%p got ar1=%lu arg2=%lu client[%u]=%p\n",
        tid(), fifo, rsp_expected, msg->arg2, client_idx, client);
    retv = 0;
  }
  ret(msg);
  DPF("%ld  wait_for_rsp:-fifo=%p rsp_expected %lu client[%u]=%p retv=%u\n",
      tid(), fifo, rsp_expected, client_idx, client, retv);
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

  printf("%ld  multi_thread_msg:+client_count=%u loops=%lu msg_count=%u\n",
      tid(), client_count, loops, msg_count);

  clock_gettime(CLOCK_REALTIME, &time_start);

  if (client_count == 0) {
    printf("%ld  multi_thread_msg: ERROR client_count=%d, aborting\n",
        tid(), client_count);
    error = true;
    goto done;
  }

  clients = malloc(sizeof(ClientParams) * client_count);
  if (clients == NULL) {
    printf("%ld  multi_thread_msg: ERROR Unable to allocate clients array, aborting\n", tid());
    error = true;
    goto done;
  }

  DPF("%ld  multi_thread_msg: init msg pool=%p\n", tid(), &pool);
  error = MsgPool_init(&pool, msg_count);
  if (error) {
    printf("%ld  multi_thread_msg: ERROR Unable to allocate messages, aborting\n", tid());
    goto done;
  }

  initMpscFifo(&cmdFifo);
  DPF("%ld  multi_thread_msg: cmdFifo=%p\n", tid(), &cmdFifo);

  // Create the clients
  for (uint32_t i = 0; i < client_count; i++, clients_created++) {
    ClientParams* param = &clients[i];
    param->msg_count = msg_count;
    param->max_peer_count = client_count;

    sem_init(&param->sem_ready, 0, 0);
    sem_init(&param->sem_waiting, 0, 0);

    int retv = pthread_create(&param->thread, NULL, client, (void*)&clients[i]);
    if (retv != 0) {
      printf("%ld  multi_thread_msg: ERROR thread creation , clients[%u]=%p retv=%d\n",
          tid(), i, param, retv);
      error = true;
      goto done;
    }

    // Wait until it starts
    sem_wait(&param->sem_ready);
  }
  DPF("%ld  multi_thread_msg: created %u clients\n", tid(), clients_created);


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
          DPF("%ld  multi_thread_msg: send client=%p msg=%p arg1=%lu CmdConnect\n",
              tid(), client, msg, msg->arg1);
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

  clock_gettime(CLOCK_REALTIME, &time_looping);

  // Loop though all the clients asking them to send to their peers
  for (uint32_t i = 0; i < loops; i++) {
    for (uint32_t c = 0; c < clients_created; c++) {
      // Test both flavors of rmv
      Msg_t* msg;
      if ((i & 1) == 0) {
        msg = rmv(&pool.fifo);
      } else {
        msg = rmv_non_stalling(&pool.fifo);
      }

      if (msg != NULL) {
        ClientParams* client = &clients[c];
        msg->arg1 = CmdSendToPeers;
        DPF("%ld  multi_thread_msg: send client=%p msg=%p arg1=%lu CmdDoNothing\n",
            tid(), client, msg, msg->arg1);
        add(&client->cmdFifo, msg);
        sem_post(&client->sem_waiting);
        msgs_sent += 1;
      } else {
        no_msgs_count += 1;
        DPF("%ld  multi_thread_msg: Whoops msg == NULL c=%u msgs_sent=%lu no_msgs_count=%lu\n",
            tid(), c, msgs_sent, no_msgs_count);
        sched_yield();
      }
    }
  }

  error = false;

done:
  clock_gettime(CLOCK_REALTIME, &time_done);

  DPF("%ld  multi_thread_msg: done, send CmdDisconnectAll %u clients\n",
      tid(), clients_created);
  uint64_t msgs_processed = 0;
  for (uint32_t i = 0; i < clients_created; i++) {
    ClientParams* client = &clients[i];

    // Request the client to stop
    Msg_t* msg = MsgPool_get_msg(&pool);
    bool once = false;
    while (msg == NULL) {
      if (!once) {
        once = true;
        DPF("%ld  multi_thread_msg: waiting for msg to send %u CmdDisconnectAll\n",
            tid(), i);
      }
      sched_yield();
      msg = MsgPool_get_msg(&pool);
    }
    msg->pRspQ = &cmdFifo;
    msg->arg1 = CmdDisconnectAll;
    DPF("%ld  multi_thread_msg: send client=%p msg=%p msg->arg1=%lu CmdDisconnectAll\n",
        tid(), client, msg, msg->arg1);
    add(&client->cmdFifo, msg);
    sem_post(&client->sem_waiting);
    if (wait_for_rsp(&cmdFifo, CmdDisconnected, client, i)) {
      error = true;
      goto done;
    }
  }

  clock_gettime(CLOCK_REALTIME, &time_disconnected);

  DPF("%ld  multi_thread_msg: done, send CmdStop %u clients\n",
      tid(), clients_created);
  for (uint32_t i = 0; i < clients_created; i++) {
    ClientParams* client = &clients[i];

    // Request the client to stop
    Msg_t* msg = MsgPool_get_msg(&pool);
    bool once = false;
    while (msg == NULL) {
      if (!once) {
        once = true;
        DPF("%ld  multi_thread_msg: waiting for msg to send %u CmdStop\n", tid(), i);
      }
      sched_yield();
      msg = MsgPool_get_msg(&pool);
    }
    DPF("%ld  multi_thread_msg: send %u CmdStop\n", tid(), i);
    msg->pRspQ = &cmdFifo;
    msg->arg1 = CmdStop;
    DPF("%ld  multi_thread_msg: send client=%p msg=%p msg->arg1=%lu CmdStop\n", tid(),
       client, msg, msg->arg1);
    add(&client->cmdFifo, msg);
    sem_post(&client->sem_waiting);
    if (wait_for_rsp(&cmdFifo, CmdStopped, client, i)) {
      error = true;
      goto done;
    }
  }

  clock_gettime(CLOCK_REALTIME, &time_stopped);

  DPF("%ld  multi_thread_msg: done, joining %u clients\n", tid(), clients_created);
  for (uint32_t i = 0; i < clients_created; i++) {
    ClientParams* client = &clients[i];
    // Wait until the thread completes
    int retv = pthread_join(client->thread, NULL);
    if (retv != 0) {
      printf("%ld  multi_thread_msg: ERROR joining failed, clients[%u]=%p retv=%d\n",
          tid(), i, (void*)client, retv);
    }

    // Cleanup resources
    sem_destroy(&client->sem_ready);
    sem_destroy(&client->sem_waiting);

    // Record if clients discovered any errors
    if (client->error_count != 0) {
      printf("%ld  multi_thread_msg: ERROR clients[%u]=%p error_count=%lu\n",
          tid(), i, (void*)client, client->error_count);
      error = true;
    }
    msgs_processed += client->msgs_processed;
    DPF("%ld  multi_thread_msg: clients[%u]=%p msgs_processed=%lu error_count=%lu\n",
        tid(), i, (void*)client, client->msgs_processed, client->error_count);
  }

  // Deinit the cmdFifo
  DPF("%ld  multi_thread_msg: deinit cmdFifo=%p\n", tid(), &cmdFifo);
  deinitMpscFifo(&cmdFifo);

  // Deinit the msg pool
  DPF("%ld  multi_thread_msg: deinit msg pool=%p\n", tid(), &pool);
  MsgPool_deinit(&pool);

  clock_gettime(CLOCK_REALTIME, &time_complete);

  uint64_t expected_value = loops * clients_created;
  uint64_t sum = msgs_sent + no_msgs_count;
  if (sum != expected_value) {
    printf("%ld  multi_thread_msg: ERROR sum=%lu != expected_value=%lu\n",
       tid(), sum, expected_value);
    error = true;
  }

  printf("%ld  multi_thread_msg: msgs_processed=%lu msgs_sent=%lu "
      "no_msgs_count=%lu\n", tid(), msgs_processed, msgs_sent, no_msgs_count);

  DPF("%ld  time_start=%lu.%lu\n", tid(), time_start.tv_sec, time_start.tv_nsec);
  DPF("%ld  time_looping=%lu.%lu\n", tid(), time_looping.tv_sec, time_looping.tv_nsec);
  DPF("%ld  time_done=%lu.%lu\n", tid(), time_done.tv_sec, time_done.tv_nsec);
  DPF("%ld  time_disconnected=%lu.%lu\n", tid(), time_disconnected.tv_sec, time_disconnected.tv_nsec);
  DPF("%ld  time_stopped=%lu.%lu\n", tid(), time_stopped.tv_sec, time_stopped.tv_nsec);
  DPF("%ld  time_complete=%lu.%lu\n", tid(), time_complete.tv_sec, time_complete.tv_nsec);

  printf("%ld  startup=%.6f\n", tid(), diff_timespec_ns(&time_looping, &time_start) / ns_flt);
  printf("%ld  looping=%.6f\n", tid(), diff_timespec_ns(&time_done, &time_looping) / ns_flt);
  printf("%ld  disconneting=%.6f\n", tid(), diff_timespec_ns(&time_disconnected, &time_done) / ns_flt);
  printf("%ld  stopping=%.6f\n", tid(), diff_timespec_ns(&time_stopped, &time_disconnected) / ns_flt);
  printf("%ld  complete=%.6f\n", tid(), diff_timespec_ns(&time_complete, &time_stopped) / ns_flt);

  uint64_t processing_ns = diff_timespec_ns(&time_complete, &time_looping);
  printf("%ld  processing=%.3fs\n", tid(), processing_ns / ns_flt);
  uint64_t msgs_per_sec = (msgs_processed * ns_u64) / processing_ns;
  printf("%ld  msgs_per_sec=%lu\n", tid(), msgs_per_sec);
  float ns_per_msg = (float)processing_ns / (float)msgs_processed;
  printf("%ld  ns_per_msg=%.1fns\n", tid(), ns_per_msg);
  printf("%ld  total=%.3f\n", tid(), diff_timespec_ns(&time_complete, &time_start) / ns_flt);

  printf("%ld  multi_thread_msg:-error=%u\n\n", tid(), error);

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
