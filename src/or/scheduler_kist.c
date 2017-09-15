/* Copyright (c) 2017, The Tor Project, Inc. */
/* See LICENSE for licensing information */

#include <event2/event.h>
#include <netinet/tcp.h>

#include "or.h"
#include "buffers.h"
#include "config.h"
#include "connection.h"
#include "networkstatus.h"
#define TOR_CHANNEL_INTERNAL_
#include "channel.h"
#include "channeltls.h"
#define SCHEDULER_PRIVATE_
#include "scheduler.h"

#define TLS_PER_CELL_OVERHEAD 29

#ifdef HAVE_KIST_SUPPORT
/* Kernel interface needed for KIST. */
#include <linux/sockios.h>
#endif /* HAVE_KIST_SUPPORT */

/*****************************************************************************
 * Data structures and supporting functions
 *****************************************************************************/

/* Indicate if we don't have the kernel support. This can happen if the kernel
 * changed and it doesn't recognized the values passed to the syscalls needed
 * by KIST. In that case, fallback to the naive approach. */
#ifdef HAVE_KIST_SUPPORT
static unsigned int kist_no_kernel_support = 0;
#endif /* HAVE_KIST_SUPPORT */

/* Socket_table hash table stuff. The socket_table keeps track of per-socket
 * limit information imposed by kist and used by kist. */

static uint32_t
socket_table_ent_hash(const socket_table_ent_t *ent)
{
  return (uint32_t)ent->chan->global_identifier;
}

static unsigned
socket_table_ent_eq(const socket_table_ent_t *a, const socket_table_ent_t *b)
{
  return a->chan->global_identifier == b->chan->global_identifier;
}

typedef HT_HEAD(socket_table_s, socket_table_ent_s) socket_table_t;

static socket_table_t socket_table = HT_INITIALIZER();

HT_PROTOTYPE(socket_table_s, socket_table_ent_s, node, socket_table_ent_hash,
             socket_table_ent_eq)
HT_GENERATE2(socket_table_s, socket_table_ent_s, node, socket_table_ent_hash,
             socket_table_ent_eq, 0.6, tor_reallocarray, tor_free_)

/* outbuf_table hash table stuff. The outbuf_table keeps track of which
 * channels have data sitting in their outbuf so the kist scheduler can force
 * a write from outbuf to kernel periodically during a run and at the end of a
 * run. */

typedef struct outbuf_table_ent_s {
  HT_ENTRY(outbuf_table_ent_s) node;
  channel_t *chan;
} outbuf_table_ent_t;

static uint32_t
outbuf_table_ent_hash(const outbuf_table_ent_t *ent)
{
  return (uint32_t)ent->chan->global_identifier;
}

static unsigned
outbuf_table_ent_eq(const outbuf_table_ent_t *a, const outbuf_table_ent_t *b)
{
  return a->chan->global_identifier == b->chan->global_identifier;
}

static outbuf_table_t outbuf_table = HT_INITIALIZER();

HT_PROTOTYPE(outbuf_table_s, outbuf_table_ent_s, node, outbuf_table_ent_hash,
             outbuf_table_ent_eq)
HT_GENERATE2(outbuf_table_s, outbuf_table_ent_s, node, outbuf_table_ent_hash,
             outbuf_table_ent_eq, 0.6, tor_reallocarray, tor_free_)

/*****************************************************************************
 * Other internal data
 *****************************************************************************/

/* Store the last time the scheduler was run so we can decide when to next run
 * the scheduler based on it. */
static monotime_t scheduler_last_run;
/* This is a factor for the extra_space calculation in kist per-socket limits.
 * It is the number of extra congestion windows we want to write to the kernel.
 */
static double sock_buf_size_factor = 1.0;
/* How often the scheduler runs. */
STATIC int32_t sched_run_interval = 10;
/* Stores the kist scheduler function pointers. */
static scheduler_t *kist_scheduler = NULL;

/*****************************************************************************
 * Internally called function implementations
 *****************************************************************************/

/* Little helper function to get the length of a channel's output buffer */
static inline size_t
channel_outbuf_length(channel_t *chan)
{
  return buf_datalen(TO_CONN(BASE_CHAN_TO_TLS(chan)->conn)->outbuf);
}

/* Little helper function for HT_FOREACH_FN. */
static int
each_channel_write_to_kernel(outbuf_table_ent_t *ent, void *data)
{
  (void) data; /* Make compiler happy. */
  channel_write_to_kernel(ent->chan);
  return 0; /* Returning non-zero removes the element from the table. */
}

/* Free the given outbuf table entry ent. */
static int
free_outbuf_info_by_ent(outbuf_table_ent_t *ent, void *data)
{
  (void) data; /* Make compiler happy. */
  log_debug(LD_SCHED, "Freeing outbuf table entry from chan=%" PRIu64,
            ent->chan->global_identifier);
  tor_free(ent);
  return 1; /* So HT_FOREACH_FN will remove the element */
}

/* Clean up outbuf_table. Probably because the KIST sched impl is going away */
static void
free_all_outbuf_info(void)
{
  HT_FOREACH_FN(outbuf_table_s, &outbuf_table, free_outbuf_info_by_ent, NULL);
}

/* Free the given socket table entry ent. */
static int
free_socket_info_by_ent(socket_table_ent_t *ent, void *data)
{
  (void) data; /* Make compiler happy. */
  log_debug(LD_SCHED, "Freeing socket table entry from chan=%" PRIu64,
            ent->chan->global_identifier);
  tor_free(ent);
  return 1; /* So HT_FOREACH_FN will remove the element */
}

/* Clean up socket_table. Probably because the KIST sched impl is going away */
static void
free_all_socket_info(void)
{
  HT_FOREACH_FN(socket_table_s, &socket_table, free_socket_info_by_ent, NULL);
}

static socket_table_ent_t *
socket_table_search(socket_table_t *table, const channel_t *chan)
{
  socket_table_ent_t search, *ent = NULL;
  search.chan = chan;
  ent = HT_FIND(socket_table_s, table, &search);
  return ent;
}

/* Free a socket entry in table for the given chan. */
static void
free_socket_info_by_chan(socket_table_t *table, const channel_t *chan)
{
  socket_table_ent_t *ent = NULL;
  ent = socket_table_search(table, chan);
  if (!ent)
    return;
  log_debug(LD_SCHED, "scheduler free socket info for chan=%" PRIu64,
            chan->global_identifier);
  HT_REMOVE(socket_table_s, table, ent);
  free_socket_info_by_ent(ent, NULL);
}

/* Perform system calls for the given socket in order to calculate kist's
 * per-socket limit as documented in the function body. */
MOCK_IMPL(void,
update_socket_info_impl, (socket_table_ent_t *ent))
{
#ifdef HAVE_KIST_SUPPORT
  int64_t tcp_space, extra_space;
  const tor_socket_t sock =
    TO_CONN(BASE_CHAN_TO_TLS((channel_t *) ent->chan)->conn)->s;
  struct tcp_info tcp;
  socklen_t tcp_info_len = sizeof(tcp);

  if (kist_no_kernel_support) {
    goto fallback;
  }

  /* Gather information */
  if (getsockopt(sock, SOL_TCP, TCP_INFO, (void *)&(tcp), &tcp_info_len) < 0) {
    if (errno == EINVAL) {
      /* Oops, this option is not provided by the kernel, we'll have to
       * disable KIST entirely. This can happen if tor was built on a machine
       * with the support previously or if the kernel was updated and lost the
       * support. */
      log_notice(LD_SCHED, "Looks like our kernel doesn't have the support "
                           "for KIST anymore. We will fallback to the naive "
                           "approach. Set KISTSchedRunInterval=-1 to disable "
                           "KIST.");
      kist_no_kernel_support = 1;
    }
    goto fallback;
  }
  if (ioctl(sock, SIOCOUTQNSD, &(ent->notsent)) < 0) {
    if (errno == EINVAL) {
      log_notice(LD_SCHED, "Looks like our kernel doesn't have the support "
                           "for KIST anymore. We will fallback to the naive "
                           "approach. Set KISTSchedRunInterval=-1 to disable "
                           "KIST.");
      /* Same reason as the above. */
      kist_no_kernel_support = 1;
    }
    goto fallback;
  }
  ent->cwnd = tcp.tcpi_snd_cwnd;
  ent->unacked = tcp.tcpi_unacked;
  ent->mss = tcp.tcpi_snd_mss;

  /* TCP space is the number of bytes would could give to the kernel and it
   * would be able to immediately push them to the network. */
  tcp_space = (ent->cwnd - ent->unacked) * ent->mss;
  if (tcp_space < 0) {
    tcp_space = 0;
  }
  /* Imagine we have filled up tcp_space already for a socket and the scheduler
   * isn't going to run again for a while. We should write a little extra to the
   * kernel so it has some data to send between scheduling runs if it gets ACKs
   * back so it doesn't sit idle. With the suggested sock_buf_size_factor of
   * 1.0, a socket can have at most 2*cwnd data in the kernel: 1 cwnd on the
   * wire waiting for ACKs and 1 cwnd ready and waiting to be sent when those
   * ACKs come. */
  extra_space =
    clamp_double_to_int64((ent->cwnd * ent->mss) * sock_buf_size_factor) -
    ent->notsent;
  if (extra_space < 0) {
    extra_space = 0;
  }
  ent->limit = tcp_space + extra_space;
  return;

#else /* HAVE_KIST_SUPPORT */
  goto fallback;
#endif /* HAVE_KIST_SUPPORT */

 fallback:
  /* If all of a sudden we don't have kist support, we just zero out all the
   * variables for this socket since we don't know what they should be.
   * We also effectively allow the socket write as much as it wants to the
   * kernel, effectively returning it to vanilla scheduler behavior. Writes
   * are still limited by the lower layers of Tor: socket blocking, full
   * outbuf, etc. */
  ent->cwnd = ent->unacked = ent->mss = ent->notsent = 0;
  ent->limit = INT_MAX;
}

/* Given a socket that isn't in the table, add it.
 * Given a socket that is in the table, reinit values that need init-ing
 * every scheduling run
 */
static void
init_socket_info(socket_table_t *table, const channel_t *chan)
{
  socket_table_ent_t *ent = NULL;
  ent = socket_table_search(table, chan);
  if (!ent) {
    log_debug(LD_SCHED, "scheduler init socket info for chan=%" PRIu64,
              chan->global_identifier);
    ent = tor_malloc_zero(sizeof(*ent));
    ent->chan = chan;
    HT_INSERT(socket_table_s, table, ent);
  }
  ent->written = 0;
}

/* Add chan to the outbuf table if it isn't already in it. If it is, then don't
 * do anything */
static void
outbuf_table_add(outbuf_table_t *table, channel_t *chan)
{
  outbuf_table_ent_t search, *ent;
  search.chan = chan;
  ent = HT_FIND(outbuf_table_s, table, &search);
  if (!ent) {
    log_debug(LD_SCHED, "scheduler init outbuf info for chan=%" PRIu64,
              chan->global_identifier);
    ent = tor_malloc_zero(sizeof(*ent));
    ent->chan = chan;
    HT_INSERT(outbuf_table_s, table, ent);
  }
}

static void
outbuf_table_remove(outbuf_table_t *table, channel_t *chan)
{
  outbuf_table_ent_t search, *ent;
  search.chan = chan;
  ent = HT_FIND(outbuf_table_s, table, &search);
  if (ent) {
    HT_REMOVE(outbuf_table_s, table, ent);
    free_outbuf_info_by_ent(ent, NULL);
  }
}

/* Set the scheduler running interval. */
static void
set_scheduler_run_interval(const networkstatus_t *ns)
{
  int32_t old_sched_run_interval = sched_run_interval;
  sched_run_interval = kist_scheduler_run_interval(ns);
  if (old_sched_run_interval != sched_run_interval) {
    log_info(LD_SCHED, "Scheduler KIST changing its running interval "
                       "from %" PRId32 " to %" PRId32,
             old_sched_run_interval, sched_run_interval);
  }
}

/* Return true iff the channel associated socket can write to the kernel that
 * is hasn't reach the limit. */
static int
socket_can_write(socket_table_t *table, const channel_t *chan)
{
  socket_table_ent_t *ent = NULL;
  ent = socket_table_search(table, chan);
  tor_assert(ent);

  int64_t kist_limit_space =
    (int64_t) (ent->limit - ent->written) /
    (CELL_MAX_NETWORK_SIZE + TLS_PER_CELL_OVERHEAD);
  return kist_limit_space > 0;
}

/* Update the channel's socket kernel information. */
static void
update_socket_info(socket_table_t *table, const channel_t *chan)
{
  socket_table_ent_t *ent = NULL;
  ent = socket_table_search(table, chan);
  tor_assert(ent);
  update_socket_info_impl(ent);
}

/* Increament the channel's socket written value by the number of bytes. */
static void
update_socket_written(socket_table_t *table, channel_t *chan, size_t bytes)
{
  socket_table_ent_t *ent = NULL;
  ent = socket_table_search(table, chan);
  tor_assert(ent);

  log_debug(LD_SCHED, "chan=%" PRIu64 " wrote %lu bytes, old was %" PRIi64,
            chan->global_identifier, bytes, ent->written);

  ent->written += bytes;
}

/*
 * A naive KIST impl would write every single cell all the way to the kernel.
 * That would take a lot of system calls. A less bad KIST impl would write a
 * channel's outbuf to the kernel only when we are switching to a different
 * channel. But if we have two channels with equal priority, we end up writing
 * one cell for each and bouncing back and forth. This KIST impl avoids that
 * by only writing a channel's outbuf to the kernel if it has 8 cells or more
 * in it.
 */
MOCK_IMPL(int, channel_should_write_to_kernel,
          (outbuf_table_t *table, channel_t *chan))
{
  outbuf_table_add(table, chan);
  /* CELL_MAX_NETWORK_SIZE * 8 because we only want to write the outbuf to the
   * kernel if there's 8 or more cells waiting */
  return channel_outbuf_length(chan) > (CELL_MAX_NETWORK_SIZE * 8);
}

/* Little helper function to write a channel's outbuf all the way to the
 * kernel */
MOCK_IMPL(void, channel_write_to_kernel, (channel_t *chan))
{
  log_debug(LD_SCHED, "Writing %lu bytes to kernel for chan %" PRIu64,
            channel_outbuf_length(chan), chan->global_identifier);
  connection_handle_write(TO_CONN(BASE_CHAN_TO_TLS(chan)->conn), 0);
}

/* Return true iff the scheduler has work to perform. */
static int
have_work(void)
{
  smartlist_t *cp = get_channels_pending();
  tor_assert(cp);
  return smartlist_len(cp) > 0;
}

/* Function of the scheduler interface: free_all() */
static void
kist_free_all(void)
{
  free_all_outbuf_info();
  free_all_socket_info();
}

/* Function of the scheduler interface: on_channel_free() */
static void
kist_on_channel_free(const channel_t *chan)
{
  free_socket_info_by_chan(&socket_table, chan);
}

/* Function of the scheduler interface: on_new_consensus() */
static void
kist_scheduler_on_new_consensus(const networkstatus_t *old_c,
                                const networkstatus_t *new_c)
{
  (void) old_c;
  (void) new_c;

  set_scheduler_run_interval(new_c);
}

/* Function of the scheduler interface: on_new_options() */
static void
kist_scheduler_on_new_options(void)
{
  sock_buf_size_factor = get_options()->KISTSockBufSizeFactor;

  /* Calls kist_scheduler_run_interval which calls get_options(). */
  set_scheduler_run_interval(NULL);
}

/* Function of the scheduler interface: init() */
static void
kist_scheduler_init(void)
{
  kist_scheduler_on_new_options();
  tor_assert(sched_run_interval > 0);
}

/* Function of the scheduler interface: schedule() */
static void
kist_scheduler_schedule(void)
{
  struct monotime_t now;
  struct timeval next_run;
  int32_t diff;
  struct event *ev = get_run_sched_ev();
  tor_assert(ev);
  if (!have_work()) {
    return;
  }
  monotime_get(&now);
  diff = (int32_t) monotime_diff_msec(&scheduler_last_run, &now);
  if (diff < sched_run_interval) {
    next_run.tv_sec = 0;
    /* 1000 for ms -> us */
    next_run.tv_usec = (sched_run_interval - diff) * 1000;
    /* Readding an event reschedules it. It does not duplicate it. */
    event_add(ev, &next_run);
  } else {
    event_active(ev, EV_TIMEOUT, 1);
  }
}

/* Function of the scheduler interface: run() */
static void
kist_scheduler_run(void)
{
  /* Define variables */
  channel_t *chan = NULL; // current working channel
  /* The last distinct chan served in a sched loop. */
  channel_t *prev_chan = NULL;
  int flush_result; // temporarily store results from flush calls
  /* Channels to be readding to pending at the end */
  smartlist_t *to_readd = NULL;
  smartlist_t *cp = get_channels_pending();

  /* For each pending channel, collect new kernel information */
  SMARTLIST_FOREACH_BEGIN(cp, const channel_t *, pchan) {
      init_socket_info(&socket_table, pchan);
      update_socket_info(&socket_table, pchan);
  } SMARTLIST_FOREACH_END(pchan);

  log_debug(LD_SCHED, "Running the scheduler. %d channels pending",
            smartlist_len(cp));

  /* The main scheduling loop. Loop until there are no more pending channels */
  while (smartlist_len(cp) > 0) {
    /* get best channel */
    chan = smartlist_pqueue_pop(cp, scheduler_compare_channels,
                                offsetof(channel_t, sched_heap_idx));
    tor_assert(chan);
    outbuf_table_add(&outbuf_table, chan);

    /* if we have switched to a new channel, consider writing the previous
     * channel's outbuf to the kernel. */
    if (!prev_chan) {
      prev_chan = chan;
    }
    if (prev_chan != chan) {
      if (channel_should_write_to_kernel(&outbuf_table, prev_chan)) {
        channel_write_to_kernel(prev_chan);
        outbuf_table_remove(&outbuf_table, prev_chan);
      }
      prev_chan = chan;
    }

    /* Only flush and write if the per-socket limit hasn't been hit */
    if (socket_can_write(&socket_table, chan)) {
      /* flush to channel queue/outbuf */
      flush_result = (int)channel_flush_some_cells(chan, 1); // 1 for num cells
      /* flush_result has the # cells flushed */
      if (flush_result > 0) {
        update_socket_written(&socket_table, chan, flush_result *
                              (CELL_MAX_NETWORK_SIZE + TLS_PER_CELL_OVERHEAD));
      }
      /* XXX What if we didn't flush? */
    }

    /* Decide what to do with the channel now */

    if (!channel_more_to_flush(chan) &&
        !socket_can_write(&socket_table, chan)) {

      /* Case 1: no more cells to send, and cannot write */

      /*
       * You might think we should put the channel in SCHED_CHAN_IDLE. And
       * you're probably correct. While implementing KIST, we found that the
       * scheduling system would sometimes lose track of channels when we did
       * that. We suspect it has to do with the difference between "can't
       * write because socket/outbuf is full" and KIST's "can't write because
       * we've arbitrarily decided that that's enough for now." Sometimes
       * channels run out of cells at the same time they hit their
       * kist-imposed write limit and maybe the rest of Tor doesn't put the
       * channel back in pending when it is supposed to.
       *
       * This should be investigated again. It is as simple as changing
       * SCHED_CHAN_WAITING_FOR_CELLS to SCHED_CHAN_IDLE and seeing if Tor
       * starts having serious throughput issues. Best done in shadow/chutney.
       */
      chan->scheduler_state = SCHED_CHAN_WAITING_FOR_CELLS;
      log_debug(LD_SCHED, "chan=%" PRIu64 " now waiting_for_cells",
                chan->global_identifier);
    } else if (!channel_more_to_flush(chan)) {

      /* Case 2: no more cells to send, but still open for writes */

      chan->scheduler_state = SCHED_CHAN_WAITING_FOR_CELLS;
      log_debug(LD_SCHED, "chan=%" PRIu64 " now waiting_for_cells",
                chan->global_identifier);
    } else if (!socket_can_write(&socket_table, chan)) {

      /* Case 3: cells to send, but cannot write */

      /*
       * We want to write, but can't. If we left the channel in
       * channels_pending, we would never exit the scheduling loop. We need to
       * add it to a temporary list of channels to be added to channels_pending
       * after the scheduling loop is over. They can hopefully be taken care of
       * in the next scheduling round.
       */
      chan->scheduler_state = SCHED_CHAN_WAITING_TO_WRITE;
      if (!to_readd) {
        to_readd = smartlist_new();
      }
      smartlist_add(to_readd, chan);
      log_debug(LD_SCHED, "chan=%" PRIu64 " now waiting_to_write",
                chan->global_identifier);
    } else {

      /* Case 4: cells to send, and still open for writes */

      chan->scheduler_state = SCHED_CHAN_PENDING;
      smartlist_pqueue_add(cp, scheduler_compare_channels,
                           offsetof(channel_t, sched_heap_idx), chan);
    }
  } /* End of main scheduling loop */

  /* Write the outbuf of any channels that still have data */
  HT_FOREACH_FN(outbuf_table_s, &outbuf_table, each_channel_write_to_kernel,
                NULL);
  free_all_outbuf_info();
  HT_CLEAR(outbuf_table_s, &outbuf_table);

  log_debug(LD_SCHED, "len pending=%d, len to_readd=%d",
            smartlist_len(cp),
            (to_readd ? smartlist_len(to_readd) : -1));

  /* Readd any channels we need to */
  if (to_readd) {
    SMARTLIST_FOREACH_BEGIN(to_readd, channel_t *, readd_chan) {
      readd_chan->scheduler_state = SCHED_CHAN_PENDING;
      if (!smartlist_contains(cp, readd_chan)) {
        smartlist_pqueue_add(cp, scheduler_compare_channels,
                             offsetof(channel_t, sched_heap_idx), readd_chan);
      }
    } SMARTLIST_FOREACH_END(readd_chan);
    smartlist_free(to_readd);
  }

  monotime_get(&scheduler_last_run);
}

/*****************************************************************************
 * Externally called function implementations not called through scheduler_t
 *****************************************************************************/

/* Return the KIST scheduler object. If it didn't exists, return a newly
 * allocated one but init() is not called. */
scheduler_t *
get_kist_scheduler(void)
{
  if (!kist_scheduler) {
    log_debug(LD_SCHED, "Allocating kist scheduler struct");
    kist_scheduler = tor_malloc_zero(sizeof(*kist_scheduler));
    kist_scheduler->free_all = kist_free_all;
    kist_scheduler->on_channel_free = kist_on_channel_free;
    kist_scheduler->init = kist_scheduler_init;
    kist_scheduler->on_new_consensus = kist_scheduler_on_new_consensus;
    kist_scheduler->schedule = kist_scheduler_schedule;
    kist_scheduler->run = kist_scheduler_run;
    kist_scheduler->on_new_options = kist_scheduler_on_new_options;
  }
  return kist_scheduler;
}

/* Check the torrc for the configured KIST scheduler run interval.
 * - If torrc < 0, then return the negative torrc value (shouldn't even be
 *   using KIST)
 * - If torrc > 0, then return the positive torrc value (should use KIST, and
 *   should use the set value)
 * - If torrc == 0, then look in the consensus for what the value should be.
 *   - If == 0, then return -1 (don't use KIST)
 *   - If > 0, then return the positive consensus value
 *   - If consensus doesn't say anything, return 10 milliseconds
 */
int32_t
kist_scheduler_run_interval(const networkstatus_t *ns)
{
  int32_t run_interval = (int32_t)get_options()->KISTSchedRunInterval;
  if (run_interval != 0) {
    log_debug(LD_SCHED, "Found KISTSchedRunInterval in torrc. Using that.");
    return run_interval;
  }

  log_debug(LD_SCHED, "Turning to the consensus for KISTSchedRunInterval");
  run_interval = networkstatus_get_param(ns, "KISTSchedRunInterval",
                                         KIST_SCHED_RUN_INTERVAL_DEFAULT,
                                         KIST_SCHED_RUN_INTERVAL_MIN,
                                         KIST_SCHED_RUN_INTERVAL_MAX);
  if (run_interval <= 0)
    return -1;
  return run_interval;
}

#ifdef HAVE_KIST_SUPPORT

/* Return true iff the scheduler subsystem should use KIST. */
int
scheduler_should_use_kist(void)
{
  int64_t run_interval = kist_scheduler_run_interval(NULL);
  log_info(LD_SCHED, "Determined sched_run_interval should be %" PRId64 ". "
                     "Will%s use KIST.",
           run_interval, (run_interval > 0 ? "" : " not"));
  return run_interval > 0;
}

#else /* HAVE_KIST_SUPPORT */

int
scheduler_should_use_kist(void)
{
  return 0;
}

#endif /* HAVE_KIST_SUPPORT */

