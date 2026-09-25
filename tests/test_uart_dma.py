from pathlib import Path
import subprocess, tempfile, unittest
ROOT=Path(__file__).resolve().parents[1]
class DmaTests(unittest.TestCase):
 def test_buffer_ownership_completion_backpressure_and_retry(self):
  source=(ROOT/'Core/Src/telemetry_uart.c').read_text()
  start=source.index('static void telemetry_uart_tx_kick_locked(')
  end=source.index('SedsResult telemetry_uart_init(',start)
  code=r'''
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>
#define TELEMETRY_UART_QUEUE_DEPTH 6U
#define TELEMETRY_UART_FRAME_SIZE 1028U
#define TELEMETRY_UART_PAYLOAD_CAPACITY 1024U
#define TELEMETRY_UART_HEADER_SIZE 4U
#define SEDS_DT_UMBILICAL_STATUS 140U
#define GW_STATUS_UART_SENT 2U
#define GW_STATUS_UART_FAILED 3U
#define HAL_OK 0
#define HAL_ERROR 1
typedef int HAL_StatusTypeDef;
typedef struct { struct {uint32_t BaudRate;} Init; } UART_HandleTypeDef;
static UART_HandleTypeDef uart={{1000000}}, other;
static struct {
 UART_HandleTypeDef *huart;
 uint8_t tx_payloads[6][1028]; size_t tx_lengths[6];
 uint8_t tx_head,tx_tail,tx_count,tx_active,tx_error,tx_attempts;
 uint32_t tx_started_ms,tx_frame_count;
} g_telemetry_uart;
static uint32_t g_gateway_uart_tx_queue_drops,g_gateway_uart_tx_enqueued,g_gateway_uart_tx_pending,g_gateway_uart_tx_high_water,g_gateway_uart_tx_frames,g_gateway_uart_tx_failures,g_gateway_uart_tx_exhausted,g_gateway_uart_tx_retry_count;
static uint32_t tick,starts,aborts;static int start_status;
static uint8_t *owned;static unsigned owned_len;
static uint32_t telemetry_uart_irq_save(void){return 0;}
static void telemetry_uart_irq_restore(uint32_t p){(void)p;}
static uint32_t HAL_GetTick(void){return tick;}
static uint32_t tx_time_get(void){return tick;}
static void gateway_status_observe(unsigned a,const uint8_t*b,size_t n,unsigned t,unsigned k){(void)a;(void)b;(void)n;(void)t;(void)k;}
static size_t telemetry_uart_build_frame(uint8_t *out,uint8_t magic,const uint8_t *p,size_t n){out[0]=magic;out[1]=0x5a;out[2]=n;out[3]=n>>8;if(n)memcpy(out+4,p,n);return n+4;}
static HAL_StatusTypeDef HAL_UART_Transmit_DMA(UART_HandleTypeDef*u,uint8_t*p,uint16_t n){assert(u==&uart);starts++;if(start_status)return start_status;assert(!owned);owned=p;owned_len=n;return HAL_OK;}
static HAL_StatusTypeDef HAL_UART_AbortTransmit(UART_HandleTypeDef*u){assert(u==&uart);aborts++;owned=NULL;return HAL_OK;}
''' + source[start:end] + r'''
static void complete(void){owned=NULL;HAL_UART_TxCpltCallback(&uart);}
int main(void){
 g_telemetry_uart.huart=&uart;
 for(uint8_t n=0;n<6;n++)assert(telemetry_uart_enqueue_frame(0xa5,&n,1));
 assert(starts==1 && owned_len==5 && owned[4]==0);
 uint8_t x=99;assert(!telemetry_uart_enqueue_frame(0xa5,&x,1));
 assert(owned[4]==0 && g_gateway_uart_tx_pending==6);
 HAL_UART_TxCpltCallback(&other);assert(g_gateway_uart_tx_pending==6);
 for(unsigned n=0;n<6;n++){assert(owned[4]==n);complete();}
 assert(!owned && g_gateway_uart_tx_pending==0 && g_gateway_uart_tx_frames==6);
 assert(g_gateway_uart_tx_high_water==6 && g_gateway_uart_tx_queue_drops==1);
 start_status=HAL_ERROR;assert(telemetry_uart_enqueue_frame(0xa5,&x,1));
 telemetry_uart_flush_tx_queue();telemetry_uart_flush_tx_queue();telemetry_uart_flush_tx_queue();
 assert(g_gateway_uart_tx_exhausted==1 && g_gateway_uart_tx_retry_count==2);
 assert(!g_telemetry_uart.tx_count && aborts==3);
 start_status=HAL_OK;tick=0xfffffff0;assert(telemetry_uart_enqueue_frame(0xa5,&x,1));
 tick=60;telemetry_uart_flush_tx_queue();assert(owned && g_gateway_uart_tx_retry_count==3);
 complete();assert(g_gateway_uart_tx_frames==7);
 assert(!telemetry_uart_enqueue_frame(0xa5,&x,1025));
}
'''
  with tempfile.TemporaryDirectory() as d:
   p=Path(d); (p/'test.c').write_text(code)
   subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(p/'test.c'),'-o',str(p/'test')],check=True)
   subprocess.run([str(p/'test')],check=True)
if __name__=='__main__':unittest.main()
