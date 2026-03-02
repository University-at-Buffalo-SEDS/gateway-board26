// fixed_version.c
#include "telemetry.h"

#include "app_threadx.h" // should bring in tx_api.h; if not, include tx_api.h directly
#include "can_bus.h"
#include "sedsprintf.h"
#include "stm32g4xx_hal.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "main.h"

/*
 * Wrapper-wide lock domain:
 * reuse telemetry_lock()/telemetry_unlock() so C wrapper + Rust router +
 * Rust allocator share one recursive lock and avoid cross-layer races.
 *
 * IMPORTANT: do not call these wrapper APIs from ISR context because the lock
 * implementation may block.
 */
extern void telemetry_lock(void);
extern void telemetry_unlock(void);

#ifndef TELEMETRY_ENABLED
static void print_data_no_telem(void *data, size_t len)
{
  (void)data;
  (void)len;
  printf("Telemetry disabled; data of length %zu dropped\n, raw data: ", len);
  uint8_t *p = (uint8_t *)data;
  for (size_t i = 0; i < len && i < 16; i++)
  {
    printf("%02x ", p[i]);
  }
  printf("\n");
}
#endif

#if defined(__GNUC__) || defined(__clang__)
#define UNUSED_VALUE __attribute__((unused))
#else
#define UNUSED_VALUE
#endif

#ifndef TELEMETRY_ENABLE_TIMESYNC_ENDPOINT
#define TELEMETRY_ENABLE_TIMESYNC_ENDPOINT 1
#endif

/* Constrained RAM default:
 * 0 => "async" logging uses immediate send path (no router queue growth)
 * 1 => keep true queued async behavior
 */
#ifndef TELEMETRY_ASYNC_USE_QUEUE
#define TELEMETRY_ASYNC_USE_QUEUE 1
#endif

static uint8_t g_can_rx_subscribed = 0;
static UNUSED_VALUE int32_t g_can_side_id = -1; // side ID returned by seds_router_add_side_serialized

#ifndef TX_TIMER_TICKS_PER_SECOND
#error "TX_TIMER_TICKS_PER_SECOND must be defined by ThreadX."
#endif

/* ---------------- Global router state ---------------- */
RouterState g_router = {.r = NULL, .created = 0, .start_time = 0};

/* ---------------- Time sync state ---------------- */
static int64_t g_master_offset_ms = 0; // ms
static uint64_t g_last_delay_ms = 0;   // ms (from last NTP response)
static int64_t g_unix_base_ms = 0;     // ms
static uint8_t g_unix_valid = 0;

/* Extend 32-bit ThreadX ticks to monotonic 64-bit milliseconds.
 * Stateless conversion to avoid shared mutable state races across threads.
 */
static uint64_t tx_raw_now_ms_locked(void)
{
  const uint32_t ticks32 = (uint32_t)tx_time_get();
  return ((uint64_t)ticks32 * 1000ULL) / (uint64_t)TX_TIMER_TICKS_PER_SECOND;
}

/* Public callback-compatible helper.
 *
 * IMPORTANT:
 * This callback is invoked by Rust internals in contexts that may already
 * hold telemetry_lock(). Re-acquiring telemetry_lock() here can deadlock on
 * some RTOS mutex configurations. Keep this path lock-free.
 */
static UNUSED_VALUE uint64_t tx_raw_now_ms(void *user)
{
  (void)user;
  return tx_raw_now_ms_locked();
}

static uint64_t telemetry_now_ms_locked(void)
{
  int64_t t = (int64_t)tx_raw_now_ms_locked() + g_master_offset_ms;
  if (t < 0)
    t = 0;
  return (uint64_t)t;
}

uint64_t telemetry_now_ms(void)
{
  return telemetry_now_ms_locked();
}

uint64_t telemetry_unix_ms(void)
{
  if (!g_unix_valid)
  {
    return 0;
  }

  int64_t t = (int64_t)telemetry_now_ms_locked() + g_unix_base_ms;
  if (t < 0)
    t = 0;
  return (uint64_t)t;
}

uint64_t telemetry_unix_s(void)
{
  return telemetry_unix_ms() / 1000ULL;
}

uint8_t telemetry_unix_is_valid(void)
{
  return g_unix_valid;
}

void telemetry_set_unix_time_ms(uint64_t unix_ms)
{
  const int64_t now = (int64_t)telemetry_now_ms_locked();
  g_unix_base_ms = (int64_t)unix_ms - now;
  g_unix_valid = 1;
}

static uint64_t node_now_since_ms(void *user);

static void compute_offset_delay(uint64_t t1, uint64_t t2, uint64_t t3, uint64_t t4,
                                 int64_t *offset_ms, uint64_t *delay_ms)
{
  const int64_t o = ((int64_t)(t2 - t1) + (int64_t)(t3 - t4)) / 2;
  const int64_t d = (int64_t)(t4 - t1) - (int64_t)(t3 - t2);
  *offset_ms = o;
  *delay_ms = (d < 0) ? 0 : (uint64_t)d;
}

/* Optional: expose these if you want to observe sync state elsewhere */
static int64_t g_last_offset_ms = 0;
static uint32_t g_master_offset_valid = 0;
/* -------------------------------------------------------------------------- */
/* TIMESYNC DEFERRED QUEUE (TIME_MASTER)                                      */
/* -------------------------------------------------------------------------- */

#if TELEMETRY_TIME_MASTER

#ifndef TIMESYNC_DEFERRED_QUEUE_DEPTH
#define TIMESYNC_DEFERRED_QUEUE_DEPTH 8
#endif

typedef struct
{
  uint64_t t3_for_ts;     /* timestamp to use in seds_router_log_ts */
  uint64_t resp_words[4]; /* {seq, t1, t2, t3} */
} TimesyncDeferredResp;

static struct
{
  TimesyncDeferredResp q[TIMESYNC_DEFERRED_QUEUE_DEPTH];
  uint32_t head; /* pop index */
  uint32_t tail; /* push index */
  uint32_t count;
} g_timesync_deferred = {0};

static void timesync_deferred_clear_locked(void)
{
  g_timesync_deferred.head = 0;
  g_timesync_deferred.tail = 0;
  g_timesync_deferred.count = 0;
}

static uint8_t timesync_deferred_push_locked(const TimesyncDeferredResp *item)
{
  if (g_timesync_deferred.count >= TIMESYNC_DEFERRED_QUEUE_DEPTH)
    return 0;

  g_timesync_deferred.q[g_timesync_deferred.tail] = *item;
  g_timesync_deferred.tail = (g_timesync_deferred.tail + 1U) % TIMESYNC_DEFERRED_QUEUE_DEPTH;
  g_timesync_deferred.count++;
  return 1;
}

static uint8_t timesync_deferred_pop_locked(TimesyncDeferredResp *out)
{
  if (g_timesync_deferred.count == 0)
    return 0;

  *out = g_timesync_deferred.q[g_timesync_deferred.head];
  g_timesync_deferred.head = (g_timesync_deferred.head + 1U) % TIMESYNC_DEFERRED_QUEUE_DEPTH;
  g_timesync_deferred.count--;
  return 1;
}

/*
 * Drain queued timesync responses and send them via the router.
 * IMPORTANT: call this from a normal thread context (not ISR),
 * and NOT from inside any router handler.
 *
 * Recommended loop order:
 *   process_rx_queue_timeout(...);  // handlers enqueue
 *   telemetry_timesync_process_queue(); // dequeue & log responses (safe)
 *   dispatch_tx_queue_timeout(...); // flush TX
 */
void telemetry_timesync_process_queue(void)
{
#ifndef TELEMETRY_ENABLED
  return;
#else
  for (;;)
  {
    TimesyncDeferredResp item;
    uint8_t have = 0;

    have = timesync_deferred_pop_locked(&item);

    if (!have)
      break;

    // SedsResult res = seds_router_log_queue_ts(
    //     g_router.r,
    //     SEDS_DT_TIME_SYNC_RESPONSE,
    //     item.t3_for_ts,
    //     item.resp_words,
    //     4);
    // if (res != SEDS_OK)
    // {
    //   /* Log error or handle failure */
    // }
  }
#endif
}

#else
/* Non-master builds: provide symbol as no-op for easy linking. */
void telemetry_timesync_process_queue(void)
{
}
#endif

/* Tuning knobs */
#ifndef NET_TIMESYNC_STEP_THRESHOLD_MS
#define NET_TIMESYNC_STEP_THRESHOLD_MS 250 /* if |offset| >= this, step immediately */
#endif

#ifndef NET_TIMESYNC_MAX_STEP_MS
#define NET_TIMESYNC_MAX_STEP_MS 30000 /* safety clamp (already in your file) */
#endif

#ifndef NET_TIMESYNC_SMOOTH_DIV
#define NET_TIMESYNC_SMOOTH_DIV 4 /* slew = offset/div */
#endif

static UNUSED_VALUE SedsResult on_timesync(const SedsPacketView *pkt, void *user)
{
  (void)user;
  if (!pkt || !pkt->payload)
    return SEDS_ERR;

  /* ------------------------------------------------------------ */
  /* TIME_SYNC_RESPONSE: client receives server timestamps         */
  /* Payload: { seq, t1_client, t2_server, t3_server } (4x u64)    */
  /* ------------------------------------------------------------ */
  if (pkt->ty == SEDS_DT_TIME_SYNC_RESPONSE && pkt->payload_len >= 32)
  {
    uint64_t t1 = 0, t2 = 0, t3 = 0;

    /* NOTE: layout matches how master queues resp_words[] */
    memcpy(&t1, pkt->payload + 8, 8);
    memcpy(&t2, pkt->payload + 16, 8);
    memcpy(&t3, pkt->payload + 24, 8);

    /* t4 = client receive time, in the SAME local/raw clock domain as t1 */
    const uint64_t t4 = tx_raw_now_ms_locked();

    int64_t offset_ms = 0;
    uint64_t delay_ms = 0;
    compute_offset_delay(t1, t2, t3, t4, &offset_ms, &delay_ms);

    g_last_delay_ms = delay_ms;

#if !TELEMETRY_TIME_MASTER
    /* Safety clamp: ignore insane jumps */
    // if (offset_ms > NET_TIMESYNC_MAX_STEP_MS || offset_ms < -NET_TIMESYNC_MAX_STEP_MS)
    //   return SEDS_OK;

    g_last_offset_ms = offset_ms;

    /*
     * Apply the offset to THIS board’s notion of "network time":
     * g_master_offset_ms is added to tx_raw_now_ms_locked() in telemetry_now_ms_locked().
     *
     * Policy:
     *  - Large offset => step immediately (converges fast)
     *  - Small offset => slew (reduces jitter)
     */
    int64_t applied = 0;

    if (offset_ms >= (int64_t)NET_TIMESYNC_STEP_THRESHOLD_MS ||
        offset_ms <= -(int64_t)NET_TIMESYNC_STEP_THRESHOLD_MS)
    {
      /* STEP: apply full correction */
      applied = offset_ms;
    }
    else
    {
      /* SLEW: apply fraction */
      applied = offset_ms / (int64_t)NET_TIMESYNC_SMOOTH_DIV;
      if (applied == 0)
        applied = (offset_ms > 0) ? 1 : (offset_ms < 0) ? -1
                                                        : 0;
    }

    if (applied != 0)
    {
      /*
       * If you maintain unix_base_ms relative to telemetry_now_ms_locked(),
       * then changing g_master_offset_ms would “jump” telemetry_unix_ms().
       * Compensate unix_base_ms by the same delta to keep unix time continuous.
       */
      if (g_unix_valid)
        g_unix_base_ms -= applied;

      g_master_offset_ms += applied;
      g_master_offset_valid = 1;
    }
#endif

    return SEDS_OK;
  }

  /* ------------------------------------------------------------ */
  /* TIME_SYNC_REQUEST: master receives client t1 and must respond */
  /* Payload: { seq, t1_client } (2x u64)                          */
  /* ------------------------------------------------------------ */
  if (pkt->ty == SEDS_DT_TIME_SYNC_REQUEST && pkt->payload_len >= 16)
  {
#if TELEMETRY_TIME_MASTER
    uint64_t seq = 0, t1 = 0;
    memcpy(&seq, pkt->payload + 0, 8);
    memcpy(&t1, pkt->payload + 8, 8);

    const uint64_t t2 = tx_raw_now_ms_locked();
    const uint64_t t3 = tx_raw_now_ms_locked();

    TimesyncDeferredResp item;
    item.t3_for_ts = t3;
    item.resp_words[0] = seq;
    item.resp_words[1] = t1;
    item.resp_words[2] = t2;
    item.resp_words[3] = t3;

    (void)timesync_deferred_push_locked(&item);
    return SEDS_OK;
#else
    return SEDS_OK;
#endif
  }

  /* ------------------------------------------------------------ */
  /* TIME_SYNC_ANNOUNCE: master sends {priority, unix_ms}          */
  /* Payload: { priority, unix_ms } (2x u64)                       */
  /* ------------------------------------------------------------ */
  if (pkt->ty == SEDS_DT_TIME_SYNC_ANNOUNCE && pkt->payload_len >= 16)
  {
#if !TELEMETRY_TIME_MASTER
    uint64_t unix_ms = 0;
    memcpy(&unix_ms, pkt->payload + 8, 8);

    const uint64_t half_delay = g_last_delay_ms / 2ULL;
    const int64_t now = (int64_t)telemetry_now_ms_locked();

    g_unix_base_ms = (int64_t)(unix_ms + half_delay) - now;
    g_unix_valid = 1;
#endif
    return SEDS_OK;
  }

  return SEDS_OK;
}

static UNUSED_VALUE uint64_t node_now_since_ms(void *user)
{
  (void)user;
  const RouterState s = g_router;
  if (!s.r)
    return 0;

  if (telemetry_unix_is_valid())
    return telemetry_unix_ms();

  return telemetry_now_ms_locked();
}

SedsResult tx_send(const uint8_t *bytes, size_t len, void *user)
{
  (void)user;

  if (!bytes || len == 0)
  {
    return SEDS_BAD_ARG;
  }

  return (can_bus_send_large(bytes, len, 0x04) == HAL_OK) ? SEDS_OK : SEDS_IO;
}

SedsResult on_sd_packet(const SedsPacketView *pkt, void *user)
{
  (void)user;
  if (!pkt)
  {
    return SEDS_BAD_ARG;
  }

  const int32_t n = seds_pkt_to_string_len(pkt);
  if (n <= 0)
  {
    printf("Error: failed to get string length for SD packet\r\n");
    return SEDS_ERR;
  }

  char buff[(size_t)n];
  const int32_t got = seds_pkt_to_string(pkt, buff, sizeof(buff));
  if (got < 0)
  {
    printf("Error: failed to convert SD packet to string\r\n");
    return SEDS_ERR;
  }

  printf("%s\r\n", buff);
  return SEDS_OK;
}

static void telemetry_can_rx(const uint8_t *data, size_t len, void *user)
{
  (void)user;
  rx_asynchronous(data, len);
}

/* Must be called with telemetry_lock held */
static SedsResult ensure_router_locked(void)
{
  if (g_router.created && g_router.r)
  {
    return SEDS_OK;
  }

  if (!g_can_rx_subscribed)
  {
    if (can_bus_subscribe_rx(telemetry_can_rx, NULL) == HAL_OK)
    {
      g_can_rx_subscribed = 1;
    }
    else
    {
      printf("Error: can_bus_subscribe_rx failed\r\n");
    }
  }

#ifndef TELEMETRY_ENABLED
  g_router.created = 1;
  return SEDS_OK;
#else
  const SedsLocalEndpointDesc locals[] = {
      {
          .endpoint = (uint32_t)SEDS_EP_SD_CARD,
          .packet_handler = on_sd_packet,
          .serialized_handler = NULL,
          .user = NULL,
      },
#if TELEMETRY_ENABLE_TIMESYNC_ENDPOINT
      {
          .endpoint = (uint32_t)SEDS_EP_TIME_SYNC,
          .packet_handler = on_timesync,
          .serialized_handler = NULL,
          .user = NULL,
      },
#endif
  };

  SedsRouter *r = seds_router_new(Seds_RM_Sink, node_now_since_ms, NULL, locals,
                                  (size_t)(sizeof(locals) / sizeof(locals[0])));

  if (!r)
  {
    printf("Error: failed to create router\r\n");
    g_router.r = NULL;
    g_router.created = 0;
    g_can_side_id = -1;
    return SEDS_ERR;
  }

  g_can_side_id = seds_router_add_side_serialized(
      r, "can", 3, tx_send, NULL,
      /*reliable_enabled=*/false);

  if (g_can_side_id < 0)
  {
    printf("Error: failed to add CAN side: %ld\r\n", (long)g_can_side_id);
    g_can_side_id = -1;
  }

  g_router.r = r;
  g_router.created = 1;
  g_router.start_time = telemetry_unix_is_valid() ? telemetry_unix_ms()
                                                  : telemetry_now_ms_locked();
  return SEDS_OK;
#endif
}

void rx_asynchronous(const uint8_t *bytes, size_t len)
{
#ifndef TELEMETRY_ENABLED
  (void)bytes;
  (void)len;
  return;
#else
  if (!bytes || len == 0)
  {
    return;
  }

  if (init_telemetry_router() != SEDS_OK || !g_router.r)
  {
    return;
  }

  SedsRouter *r = g_router.r;
  const int32_t side_id = g_can_side_id;

  if (side_id >= 0)
  {
    (void)seds_router_rx_serialized_packet_to_queue_from_side(
        r, (uint32_t)side_id, bytes, len);
  }
  else
  {
    (void)seds_router_rx_serialized_packet_to_queue(r, bytes, len);
  }
#endif
}

static UNUSED_VALUE void rx_synchronous(const uint8_t *bytes, size_t len)
{
#ifndef TELEMETRY_ENABLED
  (void)bytes;
  (void)len;
  return;
#else
  if (!bytes || len == 0)
  {
    return;
  }

  if (init_telemetry_router() != SEDS_OK || !g_router.r)
  {
    return;
  }

  SedsRouter *r = g_router.r;
  const int32_t side_id = g_can_side_id;

  if (side_id >= 0)
  {
    (void)seds_router_receive_serialized_from_side(
        r, (uint32_t)side_id, bytes, len);
  }
  else
  {
    (void)seds_router_receive_serialized(r, bytes, len);
  }
#endif
}

static UNUSED_VALUE uint64_t g_timesync_seq = 1;

SedsResult telemetry_timesync_request(void)
{
#ifndef TELEMETRY_ENABLED
  return SEDS_OK;
#else
#if !TELEMETRY_ENABLE_TIMESYNC_ENDPOINT
  return SEDS_OK;
#else
  /* Ensure router exists, but never hold wrapper lock across Rust calls. */
  if (init_telemetry_router() != SEDS_OK)
  {
    return SEDS_ERR;
  }

  SedsRouter *r = g_router.r;
  const uint64_t seq = g_timesync_seq++;

  if (r == NULL)
  {
    return SEDS_ERR;
  }

  const uint64_t t1 = tx_raw_now_ms(NULL);
  const uint64_t req[2] = {seq, t1};
  return seds_router_log_queue_ts(r, SEDS_DT_TIME_SYNC_REQUEST, t1, req, 2);
#endif
#endif
}

SedsResult init_telemetry_router(void)
{
  return ensure_router_locked();
}

static inline SedsElemKind guess_kind_from_elem_size(size_t elem_size)
{
  if (elem_size == 4 || elem_size == 8)
  {
    return SEDS_EK_FLOAT;
  }
  return SEDS_EK_UNSIGNED;
}

SedsResult log_telemetry_synchronous(SedsDataType data_type, const void *data,
                                     size_t element_count,
                                     size_t element_size)
{
#ifdef TELEMETRY_ENABLED
  if (!data || element_count == 0 || element_size == 0)
  {
    return SEDS_BAD_ARG;
  }

  if (init_telemetry_router() != SEDS_OK || !g_router.r)
  {
    return SEDS_ERR;
  }

  const SedsElemKind kind = guess_kind_from_elem_size(element_size);
  const SedsResult out = seds_router_log_typed_ex(g_router.r, data_type, data, element_count,
                                                  element_size, kind,
                                                  /*timestamp*/ NULL,
                                                  /*queue*/ 0);
  return out;
#else
  (void)data_type;
  print_data_no_telem((void *)data, element_count * element_size);
  return SEDS_OK;
#endif
}

SedsResult log_telemetry_asynchronous(SedsDataType data_type, const void *data,
                                      size_t element_count,
                                      size_t element_size)
{
#ifdef TELEMETRY_ENABLED
  if (!data || element_count == 0 || element_size == 0)
  {
    return SEDS_BAD_ARG;
  }

  if (init_telemetry_router() != SEDS_OK || !g_router.r)
  {
    return SEDS_ERR;
  }

  const SedsElemKind kind = guess_kind_from_elem_size(element_size);
  const SedsResult out = seds_router_log_typed_ex(g_router.r, data_type, data, element_count,
                                                  element_size, kind,
                                                  /*timestamp*/ NULL,
                                                  /*queue*/ TELEMETRY_ASYNC_USE_QUEUE);
  return out;
#else
  (void)data_type;
  print_data_no_telem((void *)data, element_count * element_size);
  return SEDS_OK;
#endif
}

SedsResult dispatch_tx_queue(void)
{
#ifndef TELEMETRY_ENABLED
  return SEDS_OK;
#else
  if (init_telemetry_router() != SEDS_OK || !g_router.r)
  {
    return SEDS_ERR;
  }
  return seds_router_process_tx_queue(g_router.r);
#endif
}

SedsResult process_rx_queue(void)
{
#ifndef TELEMETRY_ENABLED
  return SEDS_OK;
#else
  if (init_telemetry_router() != SEDS_OK || !g_router.r)
  {
    return SEDS_ERR;
  }
  return seds_router_process_rx_queue(g_router.r);
#endif
}

SedsResult dispatch_tx_queue_timeout(uint32_t timeout_ms)
{
#ifndef TELEMETRY_ENABLED
  return SEDS_OK;
#else
  if (init_telemetry_router() != SEDS_OK || !g_router.r)
  {
    return SEDS_ERR;
  }
  return seds_router_process_tx_queue_with_timeout(g_router.r, timeout_ms);
#endif
}

SedsResult process_rx_queue_timeout(uint32_t timeout_ms)
{
#ifndef TELEMETRY_ENABLED
  return SEDS_OK;
#else
  if (init_telemetry_router() != SEDS_OK || !g_router.r)
  {
    return SEDS_ERR;
  }
  return seds_router_process_rx_queue_with_timeout(g_router.r, timeout_ms);
#endif
}

SedsResult process_all_queues_timeout(uint32_t timeout_ms)
{
#ifndef TELEMETRY_ENABLED
  return SEDS_OK;
#else
  if (init_telemetry_router() != SEDS_OK)
  {
    return SEDS_ERR;
  }
  if (g_router.r == NULL)
  {
    return SEDS_ERR;
  }
  return seds_router_process_all_queues_with_timeout(g_router.r, timeout_ms);
#endif
}

SedsResult log_error_asynchronous(const char *fmt, ...)
{
#ifndef TELEMETRY_ENABLED
  (void)fmt;
  return SEDS_OK;
#else
  if (init_telemetry_router() != SEDS_OK || !g_router.r)
  {
    return SEDS_ERR;
  }

  va_list args;
  va_start(args, fmt);

  va_list args_copy;
  va_copy(args_copy, args);
  int len = vsnprintf(NULL, 0, fmt, args_copy);
  va_end(args_copy);

  if (len < 0)
  {
    va_end(args);
    const char *empty = "";
    return seds_router_log_string_ex(g_router.r, SEDS_DT_GENERIC_ERROR, empty, 0, NULL, 1);
  }

  if (len > 512)
  {
    len = 512;
  }

  char buf[(size_t)len + 1];
  int written = vsnprintf(buf, (size_t)len + 1, fmt, args);
  va_end(args);

  if (written < 0)
  {
    const char *empty = "";
    return seds_router_log_string_ex(g_router.r, SEDS_DT_GENERIC_ERROR, empty, 0, NULL, 1);
  }

  return seds_router_log_string_ex(g_router.r, SEDS_DT_GENERIC_ERROR, buf, (size_t)written, NULL, 1);
#endif
}

SedsResult log_error_synchronous(const char *fmt, ...)
{
#ifndef TELEMETRY_ENABLED
  (void)fmt;
  return SEDS_OK;
#else
  if (init_telemetry_router() != SEDS_OK || !g_router.r)
  {
    return SEDS_ERR;
  }

  va_list args;
  va_start(args, fmt);

  va_list args_copy;
  va_copy(args_copy, args);
  int len = vsnprintf(NULL, 0, fmt, args_copy);
  va_end(args_copy);

  if (len < 0)
  {
    va_end(args);
    const char *empty = "";
    return seds_router_log_string_ex(g_router.r, SEDS_DT_GENERIC_ERROR, empty, 0, NULL, 0);
  }

  if (len > 512)
  {
    len = 512;
  }

  char buf[(size_t)len + 1];
  int written = vsnprintf(buf, (size_t)len + 1, fmt, args);
  va_end(args);

  if (written < 0)
  {
    const char *empty = "";
    return seds_router_log_string_ex(g_router.r, SEDS_DT_GENERIC_ERROR, empty, 0, NULL, 0);
  }

  return seds_router_log_string_ex(g_router.r, SEDS_DT_GENERIC_ERROR, buf, (size_t)written, NULL, 0);
#endif
}

SedsResult print_telemetry_error(const int32_t error_code)
{
#ifndef TELEMETRY_ENABLED
  (void)error_code;
  return SEDS_OK;
#else
  const int32_t need = seds_error_to_string_len(error_code);
  if (need <= 0)
  {
    return (SedsResult)need;
  }

  char buf[(size_t)need];
  SedsResult res = seds_error_to_string(error_code, buf, sizeof(buf));
  if (res == SEDS_OK)
  {
    printf("Error: %s\r\n", buf);
  }
  else
  {
    (void)log_error_asynchronous("Error: seds_error_to_string failed: %d\r\n", (int)res);
  }
  return res;
#endif
}

void die(const char *fmt, ...)
{
  char buf[128];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);

  while (1)
  {
    printf("FATAL: %s\r\n", buf);
    HAL_Delay(1000);
  }
}
