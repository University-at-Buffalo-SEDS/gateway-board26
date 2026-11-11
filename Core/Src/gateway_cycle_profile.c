/* DWT cycle accounting for bench and simulator profiling. See
 * gateway_cycle_profile.h; this file is compiled only when
 * GATEWAY_CYCLE_PROFILE is enabled. */
#include "gateway_cycle_profile.h"
#include "can_tx_queue.h"
#include "main.h"
#include "telemetry.h"
#include "telemetry_tlsf.h"
#include "telemetry_uart.h"
#include <string.h>

#define NO_PROFILE __attribute__((no_instrument_function))

volatile gateway_cycle_slot g_gateway_cycle_profile[GCP_SLOTS]
    __attribute__((used));
volatile uint32_t g_gateway_cycle_profile_wall __attribute__((used));
volatile uint32_t g_gateway_cycle_profile_reset __attribute__((used));
volatile uint32_t g_gateway_cycle_profile_since __attribute__((used));

#ifdef GATEWAY_CYCLE_PROFILE_NESTED
typedef struct {
  const void *function;
  uint8_t slot;
} timed_function;

/* Functions whose files are built with -finstrument-functions. */
static const timed_function g_timed[] = {
    {(const void *)rx_asynchronous, GCP_CAN_INGRESS},
    {(const void *)telemetry_uart_handle_data, GCP_UART_INGRESS},
    {(const void *)telemetry_uart_tx_send, GCP_UART_TX},
    {(const void *)can_tx_queue_submit, GCP_CAN_TX},
#ifdef TELEMETRY_USE_TLSF
    {(const void *)telemetry_tlsf_malloc, GCP_ALLOC},
    {(const void *)telemetry_tlsf_free, GCP_FREE},
    {(const void *)telemetry_tlsf_admit, GCP_ADMISSION},
    {(const void *)telemetry_tlsf_admit_cached_transit, GCP_ADMISSION},
    {(const void *)telemetry_tlsf_admit_small_protected, GCP_ADMISSION},
    {(const void *)telemetry_tlsf_admit_can_control, GCP_ADMISSION},
#endif
};
static uint32_t g_started[GCP_SLOTS];
static uint8_t g_depth[GCP_SLOTS];

static NO_PROFILE int timed_slot(const void *function) {
  for (unsigned i = 0; i < sizeof g_timed / sizeof g_timed[0]; ++i)
    if (g_timed[i].function == function) return g_timed[i].slot;
  return -1;
}

/* Outermost call of a slot owns the span; nested or reentrant calls of the
 * same slot (an ISR allocating inside an allocation) are not double counted. */
NO_PROFILE void __cyg_profile_func_enter(void *function, void *call_site) {
  (void)call_site;
  const int slot = timed_slot(function);
  if (slot < 0) return;
  const uint32_t primask = __get_PRIMASK();
  __disable_irq();
  if (g_depth[slot]++ == 0U) g_started[slot] = gateway_cycle_now();
  __set_PRIMASK(primask);
}

NO_PROFILE void __cyg_profile_func_exit(void *function, void *call_site) {
  (void)call_site;
  const int slot = timed_slot(function);
  if (slot < 0) return;
  const uint32_t primask = __get_PRIMASK();
  __disable_irq();
  if (g_depth[slot] != 0U && --g_depth[slot] == 0U)
    gateway_cycle_record((unsigned)slot, g_started[slot]);
  __set_PRIMASK(primask);
}

#endif

NO_PROFILE void gateway_cycle_profile_loop(void) {
  if ((DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk) == 0U) {
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0U;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    g_gateway_cycle_profile_since = DWT->CYCCNT;
  }
  if (g_gateway_cycle_profile_reset != 0U) {
    const uint32_t primask = __get_PRIMASK();
    __disable_irq();
    memset((void *)g_gateway_cycle_profile, 0, sizeof(g_gateway_cycle_profile));
    g_gateway_cycle_profile_since = DWT->CYCCNT;
    g_gateway_cycle_profile_reset = 0U;
    __set_PRIMASK(primask);
  }
  g_gateway_cycle_profile_wall = DWT->CYCCNT;
}
