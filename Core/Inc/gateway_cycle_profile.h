#ifndef GATEWAY_CYCLE_PROFILE_H
#define GATEWAY_CYCLE_PROFILE_H
/* Per-path DWT cycle accounting. Compiled only with GATEWAY_CYCLE_PROFILE
 * (CMake -DGATEWAY_CYCLE_PROFILE=ON); every macro is empty otherwise.
 *
 * Each slot holds {count, max, total} as 32-bit words so a debugger or the
 * simulator can read them as plain words. `total` wraps after 2^32 cycles
 * (25 s of fully busy CPU at 170 MHz): read the table at least every 20 s
 * and accumulate deltas, or write g_gateway_cycle_profile_reset = 1 to clear
 * it at the next telemetry loop. g_gateway_cycle_profile_wall holds DWT
 * CYCCNT at the last loop, so (wall delta) is the elapsed-cycle denominator.
 *
 * Spans are inclusive: a thread span includes any ISR that preempted it, and
 * nested slots (marked "in ...") are also counted in their parent.
 *
 * Thread phases are timed by telemetry_thread.c's SERVICE() wrapper and the
 * ISRs by the stm32g4xx_it.c handlers. Nested slots (9-15) are filled only
 * with GATEWAY_CYCLE_PROFILE_NESTED and need no source edits: those builds
 * compile the files that own the functions with GCC
 * -finstrument-functions, and gateway_cycle_profile.c's entry/exit hooks
 * time the listed functions (about 30 extra instructions per call of any
 * function in those files, so compare shares only between builds of the
 * same kind; per-byte UART helpers are excluded). */
#include <stdint.h>

enum {
  GCP_UART_SERVICE = 0, /* telemetry_uart_process: UART RX parse + TX kick   */
  GCP_CAN_DRAIN = 1,    /* can_bus_process_rx_for: ring pop + reassembly     */
  GCP_QUEUES = 2,       /* SEDSnet queue drain: routing + side encode + send */
  GCP_DISCOVERY = 3,    /* discovery/health maintenance                      */
  GCP_TIMESYNC = 4,     /* time-sync maintenance                             */
  GCP_OTA = 5,          /* ota_stream_poll                                   */
  GCP_WAIT = 6,         /* telemetry thread blocked/yielding (not CPU work)  */
  GCP_CAN_ISR = 7,      /* FDCAN2 IT0/IT1 handlers                           */
  GCP_UART_ISR = 8,     /* USART2 + its two DMA channel handlers             */
  GCP_CAN_INGRESS = 9,  /* in CAN_DRAIN: SEDSnet CAN-side decode + enqueue   */
  GCP_UART_INGRESS = 10,/* in UART_SERVICE: SEDSnet UART-side decode+enqueue */
  GCP_UART_TX = 11,     /* in QUEUES: copy one frame into the UART DMA queue */
  GCP_CAN_TX = 12,      /* in QUEUES: can_tx_queue_submit (copy into queue)  */
  GCP_ALLOC = 13,       /* nested: telemetry_tlsf_malloc (TLSF builds)       */
  GCP_FREE = 14,        /* nested: telemetry_tlsf_free (TLSF builds)         */
  GCP_ADMISSION = 15,   /* nested: telemetry_tlsf_admit* (SEDSnet admission) */
  GCP_LOADGEN = 16,     /* in TIM6 ISR: synthetic CAN injection (loadgen)    */
  GCP_SLOTS = 17
};

typedef struct {
  uint32_t count;
  uint32_t max;
  uint32_t total;
} gateway_cycle_slot;

#ifdef GATEWAY_CYCLE_PROFILE
extern volatile gateway_cycle_slot g_gateway_cycle_profile[GCP_SLOTS];
extern volatile uint32_t g_gateway_cycle_profile_wall;
extern volatile uint32_t g_gateway_cycle_profile_reset;
/* CYCCNT when the table was last cleared: elapsed = wall - since. */
extern volatile uint32_t g_gateway_cycle_profile_since;

/* Once per telemetry loop (thread context): enables DWT on first use and
 * services g_gateway_cycle_profile_reset. */
void gateway_cycle_profile_loop(void);

static inline uint32_t gateway_cycle_now(void) {
  return *(volatile const uint32_t *)0xE0001004UL; /* DWT->CYCCNT */
}
static inline void gateway_cycle_record(unsigned slot, uint32_t start) {
  const uint32_t elapsed = gateway_cycle_now() - start;
  volatile gateway_cycle_slot *s = &g_gateway_cycle_profile[slot];
  /* Single-word read-modify-write; an ISR updating its own slot cannot
   * interleave with a thread slot. Nested same-slot use is not supported. */
  s->count++;
  s->total += elapsed;
  if (elapsed > s->max) s->max = elapsed;
}
#define GCP_BEGIN(var) const uint32_t var = gateway_cycle_now()
#define GCP_END(slot, var) gateway_cycle_record((slot), (var))
#else
#define gateway_cycle_profile_loop() ((void)0)
#define GCP_BEGIN(var) ((void)0)
#define GCP_END(slot, var) ((void)0)
#endif

#endif
