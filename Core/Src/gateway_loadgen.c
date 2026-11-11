/* Synthetic CAN ingress for throughput measurement; see gateway_loadgen.h.
 * Compiled only when GATEWAY_LOADGEN_FPS is defined. */
#include "gateway_loadgen.h"
#include "can_bus.h"
#include "gateway_crc32.h"
#include "gateway_cycle_profile.h"
#include "main.h"
#include "sedsnet_config.h"
#include <string.h>

#define LOADGEN_STD_ID 0x105U /* data band, otherwise unused sender token */
#define LOADGEN_SENTINEL 0xA5C3E1F7UL

volatile uint32_t g_gateway_loadgen_fps __attribute__((used)) = GATEWAY_LOADGEN_FPS;
volatile uint32_t g_gateway_loadgen_injected __attribute__((used));
volatile uint32_t g_gateway_loadgen_start_ms __attribute__((used)) = 3000U;
/* 0 = not ready, 1 = running, negative = template setup failed. */
volatile int32_t g_gateway_loadgen_state __attribute__((used));

static uint8_t g_frame[64];
static uint8_t g_frame_len;
static uint8_t g_payload_offset;
static uint32_t g_ready_tick;
static uint32_t g_credit; /* frames x 1000 */

static uint32_t loadgen_crc(const uint8_t *bytes, size_t len) {
  uint32_t state = UINT32_MAX;
  if (!gateway_crc32_update(state, bytes, len, &state)) {
    for (size_t i = 0; i < len; ++i) {
      state ^= bytes[i];
      for (unsigned bit = 0; bit < 8U; ++bit)
        state = (state >> 1) ^ (UINT32_C(0xedb88320) & (0U - (state & 1U)));
    }
  }
  return ~state;
}

static void put_le32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* Thread context, from the telemetry loop: the router and the CRC adapter
 * are initialised before its first iteration. */
void gateway_loadgen_poll(void) {
  if (g_gateway_loadgen_state != 0) return;
  const uint32_t endpoint = (uint32_t)SEDS_EP_GROUND_STATION;
  uint8_t payload[4];
  put_le32(payload, LOADGEN_SENTINEL);
  const SedsPacketView view = {
      .ty = (uint32_t)SEDS_DT_KG1000, .data_size = 4U,
      .sender = "LG", .sender_len = 2U,
      .endpoints = &endpoint, .num_endpoints = 1U,
      .timestamp = 1000U, .payload = payload, .payload_len = sizeof payload};
  const int32_t n = seds_pkt_pack(&view, g_frame, sizeof g_frame);
  if (n < 8 || n > 64) { g_gateway_loadgen_state = -1; return; }
  g_frame_len = (uint8_t)n;
  unsigned offset = 0U;
  while (offset + 4U <= (unsigned)n - 4U && memcmp(&g_frame[offset], payload, 4U) != 0)
    ++offset;
  if (offset + 4U > (unsigned)n - 4U) { g_gateway_loadgen_state = -2; return; }
  const uint8_t *tail = &g_frame[n - 4];
  const uint32_t packed_crc = (uint32_t)tail[0] | ((uint32_t)tail[1] << 8U) |
      ((uint32_t)tail[2] << 16U) | ((uint32_t)tail[3] << 24U);
  /* Prove the patch-and-recompute path reproduces SEDSnet's own trailer. */
  if (loadgen_crc(g_frame, (size_t)n - 4U) != packed_crc) {
    g_gateway_loadgen_state = -3; return;
  }
  g_payload_offset = (uint8_t)offset;
  g_ready_tick = HAL_GetTick();
  g_gateway_loadgen_state = 1;
}

/* 1 kHz HAL tick ISR. Bursts of fps/1000 frames per tick mimic CAN arrival
 * bursts; the remainder is carried so the long-run rate is exact. */
void gateway_loadgen_tick(void) {
  if (g_gateway_loadgen_state != 1) return;
  if ((uint32_t)(HAL_GetTick() - g_ready_tick) < g_gateway_loadgen_start_ms) return;
  GCP_BEGIN(gcp_start);
  g_credit += g_gateway_loadgen_fps;
  while (g_credit >= 1000U) {
    g_credit -= 1000U;
    const uint32_t sequence = ++g_gateway_loadgen_injected;
    put_le32(&g_frame[g_payload_offset], sequence);
    put_le32(&g_frame[g_frame_len - 4U], loadgen_crc(g_frame, g_frame_len - 4U));
    can_bus_test_inject(LOADGEN_STD_ID, g_frame, g_frame_len);
  }
  GCP_END(GCP_LOADGEN, gcp_start);
}
