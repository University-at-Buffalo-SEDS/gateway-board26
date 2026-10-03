from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def function(source, signature):
    start = source.index(signature)
    pos = source.index('{', start)
    depth, end = 1, pos + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


class UartReceiveBudgetTests(unittest.TestCase):
    def test_bounded_service_preserves_partial_chunks_and_wraparound(self):
        source = (ROOT / 'Core/Src/telemetry_uart.c').read_text()
        funcs = '\n'.join(function(source, name) for name in (
            'static void telemetry_uart_rx_ring_push_isr(',
            'static uint8_t telemetry_uart_rx_ring_pop_byte(',
            'void telemetry_uart_process('))
        code = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#define TELEMETRY_UART_RX_DMA_BUF_SIZE 512U
#define TELEMETRY_UART_RX_RING_DEPTH 8U
#define TELEMETRY_UART_RX_SERVICE_BYTES 512U
#define TELEMETRY_UART_RX_SERVICE_MS 1U
typedef struct {uint16_t len;uint8_t data[512];} TelemetryUartRxItem;
static struct {
 TelemetryUartRxItem rx_ring[8];
 unsigned rx_head,rx_tail,rx_count,rx_item_offset;
 unsigned rx_dma_drop_count,rx_dma_active;
} g_telemetry_uart;
static unsigned g_gateway_uart_rx_ring_drops;
static uint32_t tick,received,flushes,restarts,advance;
static unsigned refill;
static uint32_t telemetry_uart_irq_save(void){return 0;}
static void telemetry_uart_irq_restore(uint32_t n){(void)n;}
static uint32_t HAL_GetTick(void){return tick;}
static void telemetry_uart_start_rx_dma(void){restarts++;g_telemetry_uart.rx_dma_active=1;}
static void telemetry_uart_flush_tx_queue(void){flushes++;}
static void telemetry_uart_rx_ring_push_isr(const uint8_t*,uint16_t);
static void telemetry_uart_process_rx_byte(uint8_t byte){
 assert(byte==(uint8_t)received++);tick+=advance;
 if(refill && received%512==0){
   uint8_t next[512];for(unsigned i=0;i<512;i++)next[i]=(uint8_t)(received+i);
   telemetry_uart_rx_ring_push_isr(next,512);
 }
}
''' + funcs + r'''
int main(void){
 uint8_t data[512];for(unsigned i=0;i<512;i++)data[i]=(uint8_t)i;
 telemetry_uart_rx_ring_push_isr(NULL,1);telemetry_uart_rx_ring_push_isr(data,0);
 assert(g_telemetry_uart.rx_count==0);
 telemetry_uart_rx_ring_push_isr(data,512);
 advance=1;tick=UINT32_MAX;
 telemetry_uart_process();assert(received==1 && g_telemetry_uart.rx_item_offset==1);
 assert(g_telemetry_uart.rx_count==1 && flushes==1 && restarts==1);
 // Resume exactly at the saved byte after timer wrap, then drain to empty.
 advance=0;telemetry_uart_process();assert(received==512 && !g_telemetry_uart.rx_count);
 // A producer that continuously refills cannot keep the consumer in one call.
 refill=1;telemetry_uart_rx_ring_push_isr(data,512);
 for(unsigned n=0;n<32;n++){
   unsigned before=received;telemetry_uart_process();assert(received-before==512);
   assert(g_telemetry_uart.rx_count==1);
 }
 refill=0;telemetry_uart_process();assert(!g_telemetry_uart.rx_count);
 unsigned before=received;
 for(unsigned n=0;n<8;n++)telemetry_uart_rx_ring_push_isr(data,512);
 telemetry_uart_rx_ring_push_isr(data,512);assert(g_gateway_uart_rx_ring_drops==1);
 for(unsigned n=0;n<8;n++)telemetry_uart_process();
 assert(received-before==4096 && !g_telemetry_uart.rx_count);
}
'''
        with tempfile.TemporaryDirectory() as d:
            p = Path(d)
            (p / 'test.c').write_text(code)
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',
                            '-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
            subprocess.run([str(p/'test')],check=True)


if __name__ == '__main__':
    unittest.main()
