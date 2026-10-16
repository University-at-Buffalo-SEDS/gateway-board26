#include "main.h"
#include "telemetry_tlsf.h"
#include "tlsf.h"
#include <stdint.h>

static TX_BYTE_POOL *backing[3];
static pool_t regions[3];
static unsigned backing_count, region_count, init_attempted;
static tlsf_t allocator;
volatile uint32_t g_telemetry_tlsf_active;
volatile uint32_t g_telemetry_tlsf_init_failed;
volatile uint32_t g_telemetry_tlsf_region_bytes;
volatile uint32_t g_telemetry_tlsf_control_bytes;
volatile uint32_t g_telemetry_tlsf_live_bytes;
static uint32_t live_allocations;
/* Keep decode-sized blocks out of the small-packet allocation stream. These
 * slots are carved from the existing pool, not additional static RAM. */
#define LARGE_SLOT_BYTES 4096U
#define LARGE_SLOT_COUNT 4U
#ifndef RESERVE_BLOCK_BYTES
#define RESERVE_BLOCK_BYTES 256U
#endif
#define RESERVE_BLOCK_COUNT 64U
#define LARGE_RESERVE_BYTES (RESERVE_BLOCK_BYTES * RESERVE_BLOCK_COUNT)
static uint8_t *large_slots;
static uint64_t reserve_used;
static uint8_t reserve_lengths[RESERVE_BLOCK_COUNT];
static uint32_t reserve_live_bytes;
volatile uint32_t g_gateway_large_slot_live;
volatile uint32_t g_gateway_large_slot_peak;
volatile uint32_t g_gateway_admission_last_additional;
volatile uint32_t g_gateway_admission_last_largest;
volatile uint32_t g_gateway_admission_last_free;
#ifdef SEDS_FIRMWARE_SIM_TEST
volatile uint32_t g_gateway_admission_last_reason;
uint64_t telemetry_tlsf_reserve_mask(void) {
    const uint32_t saved = __get_PRIMASK();
    __disable_irq();
    const uint64_t mask = reserve_used;
    __set_PRIMASK(saved);
    return mask;
}
#endif

/* A fixed bitmap avoids heap headers and bounds each search to 64 blocks.
 * Only the allocation start records its length; pointers never move. */
static int reserve_find_in(size_t size, uint64_t used)
{
    if (!large_slots || size == 0U || size > RESERVE_BLOCK_BYTES * RESERVE_BLOCK_COUNT) return -1;
    const unsigned blocks = (size + RESERVE_BLOCK_BYTES - 1U) / RESERVE_BLOCK_BYTES;
    if (blocks == 1U) {
        const uint64_t free = ~used;
        const uint32_t low = (uint32_t)free, high = (uint32_t)(free >> 32U);
        if (low) return (int)__builtin_ctz(low);
        if (high) return 32 + (int)__builtin_ctz(high);
        return -1;
    }
    const uint64_t bits = UINT64_MAX >> (RESERVE_BLOCK_COUNT - blocks);
    for (unsigned i = 0; i + blocks <= RESERVE_BLOCK_COUNT; ++i)
        if ((used & (bits << i)) == 0U) return (int)i;
    return -1;
}
static int reserve_find(size_t size)
{
    return reserve_find_in(size, reserve_used);
}
static void *large_slot_allocate(size_t size)
{
    const int index = reserve_find(size);
    if (index < 0) return NULL;
    const unsigned blocks = (size + RESERVE_BLOCK_BYTES - 1U) / RESERVE_BLOCK_BYTES;
    reserve_used |= (UINT64_MAX >> (RESERVE_BLOCK_COUNT - blocks)) << (unsigned)index;
    reserve_lengths[index] = blocks;
    reserve_live_bytes += blocks * RESERVE_BLOCK_BYTES;
    ++g_gateway_large_slot_live;
    if (g_gateway_large_slot_live > g_gateway_large_slot_peak)
        g_gateway_large_slot_peak = g_gateway_large_slot_live;
    return large_slots + (unsigned)index * RESERVE_BLOCK_BYTES;
}
static int large_slot_index(void *ptr)
{
    const uintptr_t address = (uintptr_t)ptr, start = (uintptr_t)large_slots;
    if (!large_slots || address < start || address >= start + LARGE_RESERVE_BYTES)
        return -1;
    return (address - start) / RESERVE_BLOCK_BYTES;
}
volatile uint32_t g_gateway_memory_admission_drops;
volatile uint32_t g_telemetry_tlsf_peak_bytes;
volatile uint32_t g_telemetry_tlsf_failure_request;
volatile uint32_t g_telemetry_tlsf_failures;
/* Exact free-space snapshot at startup and failed allocations, not an O(n)
 * heap walk on every packet. Live bytes count occupied TLSF payload blocks. */
volatile uint32_t g_telemetry_tlsf_free_bytes;
volatile uint32_t g_telemetry_tlsf_largest_free;
volatile uint32_t g_telemetry_tlsf_free_blocks;
volatile uint32_t g_telemetry_tlsf_snapshot_count;

void telemetry_tlsf_register_pool(TX_BYTE_POOL *pool)
{
    if (!pool || init_attempted || backing_count == 3U) {
        Error_Handler();
        return;
    }
    for (unsigned i = 0; i < backing_count; ++i) {
        if (backing[i] == pool) return;
    }
    backing[backing_count++] = pool;
}
static void snapshot_block(void *ptr, size_t size, int used, void *context)
{
    (void)ptr; (void)context;
    if (!used) {
        g_telemetry_tlsf_free_bytes += size;
        ++g_telemetry_tlsf_free_blocks;
        if (size > g_telemetry_tlsf_largest_free) g_telemetry_tlsf_largest_free = size;
    }
}
static void snapshot(void)
{
    ++g_telemetry_tlsf_snapshot_count;
    g_telemetry_tlsf_free_bytes = 0;
    g_telemetry_tlsf_largest_free = 0;
    g_telemetry_tlsf_free_blocks = 0;
    for (unsigned i = 0; i < region_count; ++i)
        tlsf_walk_pool(regions[i], snapshot_block, NULL);
    if (large_slots) {
        g_telemetry_tlsf_free_bytes += LARGE_RESERVE_BYTES - reserve_live_bytes;
        unsigned run = 0;
        for (unsigned i = 0; i < RESERVE_BLOCK_COUNT; ++i) {
            if (reserve_used & (UINT64_C(1) << i)) run = 0;
            else {
                if (run == 0U) ++g_telemetry_tlsf_free_blocks;
                ++run;
                const uint32_t bytes = run * RESERVE_BLOCK_BYTES;
                if (bytes > g_telemetry_tlsf_largest_free) g_telemetry_tlsf_largest_free = bytes;
            }
        }
    }
}
static int initialize(void)
{
    if (init_attempted) return g_telemetry_tlsf_active != 0;
    init_attempted = 1;
    if (!backing_count) goto fail;
    void *control = NULL;
    const size_t alignment = tlsf_align_size();
    const size_t control_bytes = tlsf_size() + alignment - 1U;
    if (tx_byte_allocate(backing[0], &control, control_bytes, TX_NO_WAIT) != TX_SUCCESS)
        goto fail;
    allocator = tlsf_create((void *)(((uintptr_t)control + alignment - 1U) & ~(alignment - 1U)));
    if (!allocator) goto fail;
    g_telemetry_tlsf_control_bytes = control_bytes;
    for (unsigned i = 0; i < backing_count; ++i) {
        ULONG available = 0;
        if (tx_byte_pool_info_get(backing[i], TX_NULL, &available, TX_NULL,
                                 TX_NULL, TX_NULL, TX_NULL) != TX_SUCCESS) goto fail;
        /* Startup leaves one contiguous free tail in each registered pool.
         * Keep room for ThreadX's allocation header and alignment. A failed
         * takeover is fatal to this experiment; never mix pointer ownership. */
        if (available <= 4U * sizeof(void *) + tlsf_pool_overhead() + tlsf_block_size_min())
            goto fail;
        const size_t bytes = (available - 4U * sizeof(void *)) & ~(alignment - 1U);
        void *memory = NULL;
        if (tx_byte_allocate(backing[i], &memory, bytes, TX_NO_WAIT) != TX_SUCCESS) goto fail;
        pool_t region = tlsf_add_pool(allocator, memory, bytes);
        if (!region) goto fail;
        regions[region_count++] = region;
        g_telemetry_tlsf_region_bytes += bytes;
    }
    large_slots = tlsf_memalign(allocator, 8U, LARGE_RESERVE_BYTES);
    if (!large_slots) goto fail;
    /* Reserve ownership overhead/padding conservatively in admission accounting. */
    g_telemetry_tlsf_region_bytes -= tlsf_block_size(large_slots) -
        LARGE_RESERVE_BYTES + 2U * sizeof(void *);
    g_telemetry_tlsf_active = 1;
    snapshot();
    return 1;
fail:
    g_telemetry_tlsf_init_failed = 1;
    return 0;
}
void *telemetry_tlsf_malloc(size_t size)
{
    const uint32_t saved = __get_PRIMASK();
    __disable_irq();
    void *ptr = NULL;
    if (initialize() && size <= tlsf_block_size_max() - 64U) {
        /* Startup topology/replay buffers should not occupy the reserve. */
        /* Rust reserves its own alignment/header in telemetryMalloc's raw
         * allocation. CAN callers own byte storage. Extra memalign padding
         * strands small free blocks even for a ten-byte request. */
        ptr = tlsf_malloc(allocator, size ? size : 1U);
        if (!ptr) ptr = large_slot_allocate(size);
    }
    if (ptr) {
        ++live_allocations;
        const int index = large_slot_index(ptr);
        g_telemetry_tlsf_live_bytes += index >= 0 ?
            reserve_lengths[index] * RESERVE_BLOCK_BYTES : tlsf_block_size(ptr);
        if (g_telemetry_tlsf_live_bytes > g_telemetry_tlsf_peak_bytes)
            g_telemetry_tlsf_peak_bytes = g_telemetry_tlsf_live_bytes;
    } else {
        g_telemetry_tlsf_failure_request = size;
        ++g_telemetry_tlsf_failures;
        snapshot();
    }
    __set_PRIMASK(saved);
    return ptr;
}
void telemetry_tlsf_free(void *ptr)
{
    if (!ptr) return;
    const uint32_t saved = __get_PRIMASK();
    __disable_irq();
    --live_allocations;
    const int slot = large_slot_index(ptr);
    if (slot >= 0) {
        const unsigned blocks = reserve_lengths[slot];
        reserve_used &= ~((UINT64_MAX >> (RESERVE_BLOCK_COUNT - blocks)) << (unsigned)slot);
        reserve_lengths[slot] = 0U;
        --g_gateway_large_slot_live;
        reserve_live_bytes -= blocks * RESERVE_BLOCK_BYTES;
        g_telemetry_tlsf_live_bytes -= blocks * RESERVE_BLOCK_BYTES;
    } else {
        g_telemetry_tlsf_live_bytes -= tlsf_block_size(ptr);
        tlsf_free(allocator, ptr);
    }
    __set_PRIMASK(saved);
}

void telemetry_tlsf_sample(void)
{
    const uint32_t saved = __get_PRIMASK();
    __disable_irq();
    snapshot();
    __set_PRIMASK(saved);
}

/* Keep admission bounded even with a fragmented heap. The accounting below
 * conservatively includes metadata for allocated and free blocks. Checking an
 * actual aligned scratch allocation uses TLSF's bitmap lookup and bounded
 * split/coalesce operations; walking every heap block here delayed CAN RX.
 * The probe is released before returning and cannot recurse into this hook. */
static bool admit_impl(size_t additional, size_t largest, bool cached_transit, bool protected_control)
{
    const uint32_t saved = __get_PRIMASK();
    __disable_irq();
    bool allowed = initialize() != 0;
#ifdef SEDS_FIRMWARE_SIM_TEST
    uint32_t reason = 1U;
#endif
    const size_t reserve = protected_control || additional <= 512U ? 512U : 4096U;
    /* Even a small wire ACK can allocate a larger owned queue/decoder object:
     * hardware captured a 676-byte request after a 160-byte scratch estimate.
     * Preserve the smaller ACK reserve, but admit its real working block. */
    if (additional < 1024U) additional = 1024U;
    if (!cached_transit && largest != 0U && largest < 1024U) largest = 1024U;
    /* Count actual free blocks instead of charging two headers per live
     * allocation. The former bound double-counted occupied metadata and
     * stranded free space under sustained small-packet traffic. Include the
     * separately reserved bitmap pool exactly once; keep the safety reserve
     * and contiguous scratch preflight below. No heap walk runs on ingress. */
    const size_t available = allowed ? tlsf_free_size(allocator) +
        RESERVE_BLOCK_BYTES * RESERVE_BLOCK_COUNT - reserve_live_bytes : 0U;
    allowed = allowed && additional <= available && reserve <= available - additional;
    if (allowed && largest != 0U) {
        if (largest > tlsf_block_size_max() - 64U) {
            allowed = false;
#ifdef SEDS_FIRMWARE_SIM_TEST
            reason = 2U;
#endif
        } else {
            /* A discovery snapshot clones many small strings, not just its
             * largest block. If TLSF cannot supply scratch, the fallback
             * must cover its largest buffer. Ordinary fragmented blocks
             * may hold the rest of the operation and the control reserve;
             * requiring both reserves in this pool strands usable memory. */
            const size_t reserve_free = RESERVE_BLOCK_BYTES * RESERVE_BLOCK_COUNT - reserve_live_bytes;
            const bool slot_available = reserve_find(largest) >= 0 &&
                largest <= reserve_free;
            const size_t padding = slot_available ?
                ((largest + RESERVE_BLOCK_BYTES - 1U) / RESERVE_BLOCK_BYTES) * RESERVE_BLOCK_BYTES - largest : 0U;
            if (tlsf_can_memalign(allocator, tlsf_align_size(), largest)) {
                allowed = true;
            } else {
                allowed = slot_available && padding <= available - additional - reserve;
#ifdef SEDS_FIRMWARE_SIM_TEST
                reason = slot_available ? 4U : 3U;
#endif
                /* Hold the work's small allocations while testing fragmented
                 * storage, then verify the full safety reserve remains free.
                 * No real reserve bitmap or allocation counters are changed. */
                void *head = NULL;
                uint64_t trial_used = reserve_used;
                const int index = reserve_find(largest);
                if (allowed) {
                    const unsigned blocks = (largest + RESERVE_BLOCK_BYTES - 1U) / RESERVE_BLOCK_BYTES;
                    trial_used |= (UINT64_MAX >> (RESERVE_BLOCK_COUNT - blocks)) << (unsigned)index;
                }
                /* Probe actual work only. The safety reserve must remain free;
                 * materializing it as 128-byte objects rounds every 128 bytes
                 * to a 256-byte fallback slot and double-charges headroom. */
                size_t remaining = additional > largest ? additional - largest : 0U;
                unsigned probes = 0U;
                while (allowed && remaining != 0U && probes++ < 128U) {
                    const size_t chunk = remaining > 128U ? 128U : remaining;
                    void *block = tlsf_malloc(allocator,
                        chunk < sizeof(void *) ? sizeof(void *) : chunk);
                    if (block) {
                        *(void **)block = head;
                        head = block;
                    } else {
                        /* The remainder consists of actual small objects,
                         * not one large buffer. Charge each fallback block,
                         * including its padding, in the private trial bitmap.
                         * Keep the real bitmap and allocation counters intact. */
                        const int small = reserve_find_in(chunk, trial_used);
                        if (small < 0) {
#ifdef SEDS_FIRMWARE_SIM_TEST
                            reason = 5U;
#endif
                            allowed = false; break;
                        }
                        trial_used |= UINT64_C(1) << (unsigned)small;
                    }
                    remaining -= chunk;
                }
                if (remaining != 0U) allowed = false;
                if (allowed) {
                    const size_t trial_free = tlsf_free_size(allocator) +
                        (RESERVE_BLOCK_COUNT - (unsigned)__builtin_popcountll(trial_used)) *
                            RESERVE_BLOCK_BYTES;
                    if (trial_free < reserve) {
                        allowed = false;
#ifdef SEDS_FIRMWARE_SIM_TEST
                        reason = 6U;
#endif
                    }
                }
                while (head) {
                    void *next = *(void **)head;
                    tlsf_free(allocator, head);
                    head = next;
                }
            }
        }
    }
    if (!allowed) {
#ifdef SEDS_FIRMWARE_SIM_TEST
        g_gateway_admission_last_reason = reason;
#endif
        ++g_gateway_memory_admission_drops;
        g_gateway_admission_last_free = available;
        g_gateway_admission_last_additional = additional;
        g_gateway_admission_last_largest = largest;
    }
    __set_PRIMASK(saved);
    return allowed;
}

/* Ordinary/cold decoder work retains the hardware-tested 1 KiB floor. */
bool telemetry_tlsf_admit(size_t additional, size_t largest) {
    return admit_impl(additional, largest, false, false);
}
bool telemetry_tlsf_admit_cached_transit(size_t additional, size_t largest) {
    if (additional < 1024U || additional > 1280U || largest == 0U || largest > 256U)
        return admit_impl(additional, largest, false, false);
    /* The extended network probe verifies warmed metadata before supplying
     * this bound. Keep the same 4 KiB free-space safety reserve. */
    return admit_impl(additional, 256U, true, false);
}

/* The SDK only selects this class for <=128-byte validated fixed-shape
 * reliable control/status frames. Preserve their full working estimate and
 * 1 KiB contiguous floor, but allow use of the protected control headroom. */
bool telemetry_tlsf_admit_small_protected(size_t additional, size_t largest) {
    if (additional < 2048U || additional > 2432U || largest == 0U || largest > 160U)
        return admit_impl(additional, largest, false, false);
    return admit_impl(additional, largest, false, true);
}

/* Called only for a priority CAN frame, including queue metadata, <=160 B. */
bool telemetry_tlsf_admit_can_control(size_t bytes) {
    return bytes != 0U && bytes <= 160U &&
        telemetry_tlsf_admit_small_protected(2048U + 2U * bytes, bytes);
}
