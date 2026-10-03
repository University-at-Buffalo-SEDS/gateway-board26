import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class AllocatorReserveTests(unittest.TestCase):
    def test_large_requests_use_reserve_and_fallback_without_waiting(self):
        source = (ROOT / 'Core/Src/telemetry_hooks.c').read_text()
        allocator = source[source.index('void *telemetryMalloc('):source.index('void telemetryFree(')]
        code = r'''
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
typedef unsigned UINT;
typedef unsigned long ULONG;
typedef int TX_BYTE_POOL;
#define TX_NO_MEMORY 1
#define TX_SUCCESS 0
#define TX_NO_WAIT 0
#define TX_NULL NULL
static TX_BYTE_POOL main_pool, reserve_pool;
static TX_BYTE_POOL *rust_byte_pool_external = &main_pool;
static TX_BYTE_POOL *rust_emergency_byte_pool_external = &reserve_pool;
static unsigned calls, main_fail, reserve_fail, sequence[4];
static uint32_t g_telemetry_last_alloc_request, g_telemetry_max_alloc_request;
static uint32_t g_telemetry_alloc_emergency_recoveries, g_telemetry_alloc_failure_request;
static uint32_t g_telemetry_alloc_fail, g_telemetry_alloc_count;
static ULONG g_telemetry_alloc_failure_available, g_telemetry_alloc_failure_fragments;
static void telemetry_memory_profile_sample(void) {}
static UINT tx_byte_pool_info_get(TX_BYTE_POOL *pool, void *name, ULONG *available,
    ULONG *fragments, void *first, void *count, void *next) {
    (void)pool; (void)name; (void)first; (void)count; (void)next;
    *available=9332; *fragments=181; return TX_SUCCESS;
}
static UINT tx_byte_allocate(TX_BYTE_POOL *pool, void **ptr, size_t size, UINT wait) {
    assert(wait == TX_NO_WAIT); assert(size > 0);
    unsigned reserve = pool == &reserve_pool;
    sequence[calls++] = reserve;
    if (reserve ? reserve_fail : main_fail) return TX_NO_MEMORY;
    *ptr=pool; return TX_SUCCESS;
}
''' + allocator + r'''
int main(void) {
    /* The captured 3104-byte failure must use a contiguous reserve first. */
    main_fail=1;
    assert(telemetryMalloc(3104)==&reserve_pool && calls==1 && sequence[0]==1);
    assert(g_telemetry_alloc_fail==0);
    calls=0; main_fail=0;
    assert(telemetryMalloc(2048)==&main_pool && calls==1 && sequence[0]==0);
    calls=0; reserve_fail=1;
    assert(telemetryMalloc(3104)==&main_pool && calls==2 && sequence[0]==1 && sequence[1]==0);
    calls=0; reserve_fail=0; main_fail=1;
    assert(telemetryMalloc(64)==&reserve_pool && calls==2 && sequence[0]==0 && sequence[1]==1);
    calls=0; reserve_fail=1;
    assert(telemetryMalloc(3104)==NULL && calls==2);
    assert(g_telemetry_alloc_fail==1 && g_telemetry_alloc_failure_request==3104);
    assert(g_telemetry_alloc_failure_available==9332 && g_telemetry_alloc_failure_fragments==181);
    calls=0; main_fail=0; rust_emergency_byte_pool_external=NULL;
    assert(telemetryMalloc(0)==&main_pool && calls==1);
    calls=0; rust_byte_pool_external=NULL;
    assert(telemetryMalloc(32)==NULL && calls==0);
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            binary = str(pathlib.Path(tmp) / 'allocator-reserve')
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-x', 'c', '-', '-o', binary],
                           input=code, text=True, check=True)
            subprocess.run([binary], check=True)
