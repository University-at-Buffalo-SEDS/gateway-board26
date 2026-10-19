/* Board-owned IEEE CRC acceleration. SEDSnet and wire format stay generic. */
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#ifndef GATEWAY_CRC_TEST
#include "main.h"
#endif
volatile uint32_t g_gateway_crc_hw_state; /* 0 untested, 1 ready, 2 fallback */
volatile uint32_t g_gateway_crc_hw_calls;

#ifndef GATEWAY_CRC_TEST
static uint32_t gateway_crc_hw_update(uint32_t state, const uint8_t *bytes, size_t len)
{
    CRC->POL = 0x04c11db7U;
    CRC->INIT = __RBIT(state);
    CRC->CR = CRC_CR_REV_IN | CRC_CR_REV_OUT | CRC_CR_RESET;
    while (len >= 4U) {
        uint32_t word;
        memcpy(&word, bytes, sizeof(word)); /* permits unaligned input */
        CRC->DR = word;
        bytes += 4U;
        len -= 4U;
    }
    CRC->CR = CRC_CR_REV_IN_0 | CRC_CR_REV_OUT;
    volatile uint8_t *const input = (volatile uint8_t *)&CRC->DR;
    for (size_t i = 0U; i < len; ++i) *input = bytes[i];
    return CRC->DR;
}
#endif
/* Every hardware update restores an explicit CRC state, so independent callers
 * may run between chunks. Bound interrupt masking instead of holding it across
 * a complete schema/frame. Respect an already-masked caller's interrupt state. */
int32_t gateway_crc32_update(uint32_t state, const uint8_t *bytes, size_t len,
                            uint32_t *result)
{
    enum { CRC_IRQ_CHUNK_BYTES = 256U };
    if (result == NULL || (len != 0U && bytes == NULL) || len > 4096U) return 0;
    uint32_t accumulated = state;
    size_t remaining = len;
    do {
        const size_t take = remaining > CRC_IRQ_CHUNK_BYTES ? CRC_IRQ_CHUNK_BYTES : remaining;
        const uint32_t mask = __get_PRIMASK();
        __disable_irq();
        if (g_gateway_crc_hw_state == 0U) {
            static const uint8_t check[] = "123456789";
            __HAL_RCC_CRC_CLK_ENABLE();
            const uint32_t one = gateway_crc_hw_update(UINT32_MAX, check, 9U);
            const uint32_t split = gateway_crc_hw_update(
                gateway_crc_hw_update(UINT32_MAX, check, 4U), check + 4U, 5U);
            g_gateway_crc_hw_state =
                one == 0x340bc6d9U && split == one &&
                gateway_crc_hw_update(0x13579bdfU, check, 0U) == 0x13579bdfU ? 1U : 2U;
        }
        const int32_t ok = g_gateway_crc_hw_state == 1U;
        if (ok) {
            accumulated = gateway_crc_hw_update(accumulated, bytes, take);
            if (take == remaining) ++g_gateway_crc_hw_calls;
        }
        __set_PRIMASK(mask);
        if (!ok) return 0;
        if (take != 0U) bytes += take;
        remaining -= take;
    } while (remaining != 0U);
    *result = accumulated;
    return 1;
}
