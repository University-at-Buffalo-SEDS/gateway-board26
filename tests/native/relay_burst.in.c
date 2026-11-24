/* Real SDK + actual flight_state_cache.c: the gateway relays a burst of station
 * FLIGHT_STATE writes from its UART side onto the fill CAN, configured as in
 * telemetry.c (both sides without hop receipts, ipv6-like side transport,
 * 250 ms x 17 end-to-end retries). After discovery one CAN owner (AB) is no
 * longer heard (powered down, bus fault, stalled): the gateway still counts it
 * as an owner until its route expires, so each write misses its receipt for the
 * whole retry window. The gateway acknowledged each write to the station when it
 * accepted it, so the station never re-sends; every write must still reach VB
 * and DAQ, in order. With SEDSnet 7.0.0 the writes queued behind each exhausted
 * one were dropped: 1 s apart VB/DAQ applied 2 of 8, 100 ms apart 1 of 8.
 * Transport is ideal and in-process; no CAN timing is modelled. */
#include "sedsnet.h"
#include "sedsnet_config.h"
#include "flight_state_cache.h"
#include "persistent_store.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

volatile uint32_t g_telemetry_discovery_seen;
volatile uint32_t g_reliable_refusal_count;
static uint64_t clock_ms = 1000;
static uint8_t stored = 1;
uint32_t HAL_GetTick(void) { return (uint32_t)clock_ms; }
int persistent_store_init(void) { return 0; }
int persistent_store_get(uint32_t key, void *p, size_t *n)
{ assert(key == 0x46535445U && *n == 1); *(uint8_t *)p = stored; return 0; }
int persistent_store_set(uint32_t key, const void *p, size_t n)
{ assert(key == 0x46535445U && n == 1); stored = *(const uint8_t *)p; return 0; }
uint8_t board_recovery_managed_update_allowed(const SedsPacketView *packet)
{ assert(packet); return 1U; }

#include CACHE_SOURCE

enum { GS, GB, AB, VB, DAQ, NODES };
static const char *NAMES[NODES] = {"GS", "GB", "AB", "VB", "DAQ"};
#define MAX_FRAMES 512
#define MAX_FRAME 1100
typedef struct {
  int node;
  int32_t id;
  int peers[4];
  int npeers;
  size_t count;
  uint16_t len[MAX_FRAMES];
  uint8_t data[MAX_FRAMES][MAX_FRAME];
} side_t;
static side_t sides[6];
static SedsRouter *routers[NODES];
static uint8_t applied[NODES][32];
static bool ab_silent;
static size_t napplied[NODES];

static uint64_t now(void *u) { (void)u; return clock_ms; }

static SedsResult tx(const uint8_t *bytes, size_t len, void *user)
{
  side_t *s = user;
  if (s->count == MAX_FRAMES || len > MAX_FRAME) return SEDS_IO;
  memcpy(s->data[s->count], bytes, len);
  s->len[s->count++] = (uint16_t)len;
  return SEDS_OK;
}

static SedsResult on_update(const SedsPacketView *p, void *user)
{
  const int node = (int)(intptr_t)user;
  assert(p->payload_len == 1);
  if (napplied[node] < sizeof applied[node]) applied[node][napplied[node]++] = p->payload[0];
  return SEDS_OK;
}

static void add_side(int index, int node, const char *name, size_t max_frame, size_t templates)
{
  side_t *s = &sides[index];
  s->node = node;
  s->id = seds_router_add_side_packed_profile(routers[node], name, strlen(name), tx, s, false,
                                              SEDS_SIDE_TRANSPORT_PROFILE_IPV6_LIKE, max_frame, 0U,
                                              templates);
  assert(s->id >= 0);
}

static void deliver(void)
{
  static uint8_t frame[MAX_FRAME];
  for (int i = 0; i < 6; ++i) {
    side_t *s = &sides[i];
    if (ab_silent && s->node == AB) s->count = 0; /* nothing AB sends arrives */
    for (size_t k = 0; k < s->count; ++k) {
      const size_t len = s->len[k];
      memcpy(frame, s->data[k], len);
      for (int j = 0; j < s->npeers; ++j) {
        side_t *to = &sides[s->peers[j]];
        (void)seds_router_receive_packed_from_side(routers[to->node], (uint32_t)to->id, frame, len);
      }
    }
    s->count = 0;
  }
}

static void step(void)
{
  clock_ms += 10;
  for (int n = 0; n < NODES; ++n) (void)seds_router_periodic(routers[n], 0);
  deliver();
  for (int n = 0; n < NODES; ++n) (void)seds_router_process_all_queues(routers[n]);
  deliver();
}

static void run(uint64_t ms) { for (uint64_t t = 0; t < ms; t += 10) step(); }

static void write_state(uint8_t value)
{
  uint32_t endpoint = SEDS_EP_FLIGHT_STATE;
  SedsPacketView p = {.ty = SEDS_DT_FLIGHT_STATE, .data_size = 1, .sender = "GS", .sender_len = 2,
                      .endpoints = &endpoint, .num_endpoints = 1, .timestamp = clock_ms,
                      .payload = &value, .payload_len = 1};
  uint8_t bytes[256];
  const int32_t len = seds_pkt_pack(&p, bytes, sizeof bytes);
  assert(len > 0);
  assert(seds_router_set_network_variable_packed(routers[GS], bytes, (size_t)len) == SEDS_OK);
}

static void report(const char *label)
{
  printf("%s:", label);
  for (int n = GB; n < NODES; ++n) {
    printf(" %s=[", NAMES[n]);
    for (size_t k = 0; k < napplied[n]; ++k) printf(k ? ",%u" : "%u", applied[n][k]);
    printf("]");
  }
  printf("\n");
}

int main(int argc, char **argv)
{
  setbuf(stdout, NULL);
  assert(argc == 3);
  assert(seds_schema_register_json_file(argv[1], strlen(argv[1])) == SEDS_OK);
  const unsigned spacing_ms = (unsigned)strtoul(argv[2], NULL, 10);
  /* telemetry.c: the gateway's end-to-end retry budget (one process-wide
   * setting here, so every node uses it). */
  SedsRuntimeTuningConfig tuning;
  assert(seds_get_runtime_tuning_config(&tuning) == SEDS_OK);
  tuning.reliable_retransmit_ms = 250U;
  tuning.reliable_max_retries = 17U;
  assert(seds_set_runtime_tuning_config(&tuning) == SEDS_OK);

  for (int n = 0; n < NODES; ++n) {
    routers[n] = seds_router_new(now, NULL, NULL, 0);
    assert(routers[n]);
    assert(seds_router_set_sender_id(routers[n], NAMES[n], strlen(NAMES[n])) == SEDS_OK);
    if (n == GB) {
      assert(flight_state_cache_init(routers[n]) == SEDS_OK);
      continue;
    }
    assert(seds_router_enable_network_variable(routers[n], SEDS_DT_FLIGHT_STATE, true, n == GS) == SEDS_OK);
    if (n != GS)
      assert(seds_router_on_network_variable_update(routers[n], SEDS_DT_FLIGHT_STATE, on_update,
                                                    (void *)(intptr_t)n) == SEDS_OK);
  }
  /* The gateway's own applies are counted from its cache's update counter. */
  add_side(0, GS, "uart", 1024, 16);
  add_side(1, GB, "uart", 1024, 16);
  add_side(2, GB, "can", 128, 32);
  add_side(3, AB, "can", 128, 32);
  add_side(4, VB, "can", 128, 32);
  add_side(5, DAQ, "can", 128, 32);
  sides[0].peers[0] = 1; sides[0].npeers = 1;
  sides[1].peers[0] = 0; sides[1].npeers = 1;
  for (int i = 2; i < 6; ++i)
    for (int j = 2; j < 6; ++j)
      if (i != j) sides[i].peers[sides[i].npeers++] = j;

  for (int n = 0; n < NODES; ++n) assert(seds_router_announce_discovery(routers[n]) == SEDS_OK);
  run(3000);
  ab_silent = true; /* AB was discovered; from now on nothing it sends arrives */

  static const uint8_t values[8] = {2, 3, 4, 5, 6, 5, 4, 1};
  for (int i = 0; i < 8; ++i) {
    write_state(values[i]);
    if (i < 7) run(spacing_ms);
  }
  /* Two of the gateway's exhaustion windows (250 ms x 18) after the last write. */
  run(10000);
  report("applied");
  printf("gb_updates=%u\n", (unsigned)g_flight_state_cache_updates);
  assert(g_flight_state_cache_updates == 8 && flight_state_cache_value() == 1);
  for (int n = VB; n <= DAQ; ++n) {
    if (napplied[n] != 8 || memcmp(applied[n], values, 8) != 0) {
      printf("FAIL %s missed writes queued behind the silent owner\n", NAMES[n]);
      return 1;
    }
  }
  for (int n = 0; n < NODES; ++n) seds_router_free(routers[n]);
  printf("PASS %u\n", spacing_ms);
  return 0;
}
