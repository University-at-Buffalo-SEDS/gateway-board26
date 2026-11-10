/* Real SDK + actual flight_state_cache.c: a delivered reliable FLIGHT_STATE
 * that this board deliberately refuses must still be acknowledged end to end,
 * otherwise the station retransmits it forever. Storage and the recovery
 * authority decision are explicit fixture inputs; no hardware is modelled. */
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
static unsigned writes, acks;
static uint8_t stored = 5;
static bool allowed = true, fail_write;
uint32_t HAL_GetTick(void) { return (uint32_t)clock_ms; }
int persistent_store_init(void) { return 0; }
int persistent_store_get(uint32_t key, void *p, size_t *n)
{ assert(key == 0x46535445U && *n == 1); *(uint8_t *)p = stored; return 0; }
int persistent_store_set(uint32_t key, const void *p, size_t n)
{ assert(key == 0x46535445U && n == 1); writes++; if (fail_write) return 2; stored = *(const uint8_t *)p; return 0; }
uint8_t board_recovery_managed_update_allowed(const SedsPacketView *packet)
{ assert(packet); return allowed; }

#include CACHE_SOURCE

static uint64_t now(void *u) { (void)u; return clock_ms; }
static SedsResult inert(const SedsPacketView *p, void *u) { (void)p; (void)u; return SEDS_OK; }
static SedsResult side(const SedsPacketView *p, void *u)
{ (void)u; if (p->sender_len >= 7 && !memcmp(p->sender, "E2EACK:", 7)) acks++; return SEDS_OK; }

int main(int argc, char **argv)
{
  setbuf(stdout, NULL);
  assert(argc == 3);
  assert(seds_schema_register_json_file(argv[1], strlen(argv[1])) == SEDS_OK);
  const char *mode = argv[2];
  SedsLocalEndpointDesc local = {.endpoint = SEDS_EP_BOARD_RECOVERY, .packet_handler = inert};
  SedsRouter *r = seds_router_new(now, NULL, &local, 1);
  assert(r);
  assert(seds_router_set_sender_id(r, "GB", 2) == SEDS_OK);
  assert(flight_state_cache_init(r) == SEDS_OK);
  assert(flight_state_cache_value() == 5);
  int32_t sid = seds_router_add_side_packet(r, "uart", 4, side, NULL, false);
  assert(sid >= 0);
  uint8_t value = 3;
  uint32_t endpoint = SEDS_EP_FLIGHT_STATE;
  SedsPacketView p = {.ty = SEDS_DT_FLIGHT_STATE, .data_size = 1, .sender = "GS", .sender_len = 2,
                      .endpoints = &endpoint, .num_endpoints = 1, .timestamp = 900,
                      .payload = &value, .payload_len = 1};
  bool refused = !strcmp(mode, "refused"), malformed = !strcmp(mode, "malformed");
  allowed = !refused;
  if (malformed) value = 16;
  fail_write = !strcmp(mode, "persist_fail");
  SedsResult received = seds_router_receive_from_side(r, (uint32_t)sid, &p);
  SedsResult processed = seds_router_process_all_queues(r);
  printf("mode=%s receive=%d process=%d acks=%u writes=%u value=%u refusals=%u\n", mode,
         received, processed, acks, writes, flight_state_cache_value(),
         (unsigned)g_reliable_refusal_count);
  if (refused || malformed) {
    /* Deliberately not applied, never persisted, observation retired... */
    assert(writes == 0 && stored == 5 && flight_state_cache_value() == 5);
    assert(seds_router_cached_managed_variable_packed_len(r, SEDS_DT_FLIGHT_STATE) <= 0);
    /* ...but delivered: acknowledged so the station stops retransmitting. */
    assert(received == SEDS_OK && processed == SEDS_OK && acks == 1);
    assert(g_reliable_refusal_count == 1);
    /* A retransmitted copy is not applied either and raises no error. */
    clock_ms += 250;
    assert(seds_router_receive_from_side(r, (uint32_t)sid, &p) == SEDS_OK);
    assert(seds_router_process_all_queues(r) == SEDS_OK);
    assert(writes == 0 && flight_state_cache_value() == 5);
  } else if (fail_write) {
    /* A storage failure after the apply is acknowledged (a withheld receipt
     * stalled the station's ordered FLIGHT_STATE stream); the poll retries
     * durability once per second. */
    assert(received == SEDS_OK && acks == 1 && flight_state_cache_value() == 3 &&
           g_reliable_refusal_count == 0 && stored == 5);
    fail_write = 0; clock_ms += 1000;
    assert(flight_state_cache_poll(r) == SEDS_OK && stored == 3);
  } else {
    assert(!strcmp(mode, "allowed"));
    assert(received == SEDS_OK && acks == 1 && writes == 1 && stored == 3 &&
           flight_state_cache_value() == 3 && g_reliable_refusal_count == 0);
  }
  seds_router_free(r);
  printf("PASS %s\n", mode);
  return 0;
}
