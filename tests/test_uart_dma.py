from pathlib import Path
import subprocess, tempfile, unittest
ROOT=Path(__file__).resolve().parents[1]
class DmaTests(unittest.TestCase):
 def test_buffer_ownership_completion_backpressure_and_retry(self):
  source=(ROOT/'Core/Src/telemetry_uart.c').read_text()
  start=source.index('static uint8_t *telemetry_uart_tx_buffer(')
  end=source.index('SedsResult telemetry_uart_init(',start)
  code=r'''
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>
#define TELEMETRY_UART_SMALL_DEPTH 16U
#define TELEMETRY_UART_LARGE_DEPTH 2U
#define TELEMETRY_UART_SMALL_FRAME_SIZE 128U
#define TELEMETRY_UART_QUEUE_DEPTH 18U
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
 uint8_t tx_small[16][128],tx_large[2][1028];
 uint16_t tx_lengths[18];uint8_t tx_buffers[18];uint32_t tx_buffer_used;
 uint8_t tx_head,tx_tail,tx_count,tx_active,tx_error,tx_attempts;
 uint32_t tx_started_ms,tx_frame_count;
} g_telemetry_uart;
static uint32_t g_gateway_uart_tx_queue_drops,g_gateway_uart_tx_enqueued,g_gateway_uart_tx_pending,g_gateway_uart_tx_high_water,g_gateway_uart_tx_frames,g_gateway_uart_tx_failures,g_gateway_uart_tx_exhausted,g_gateway_uart_tx_retry_count;
#ifdef SEDS_FIRMWARE_SIM_TEST
static uint32_t g_sim_uart_umbilical_status_count;
static uint32_t sim_probe_packed_data_type(const uint8_t *p,size_t len){assert(p && len==1);return SEDS_DT_UMBILICAL_STATUS;}
#endif
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
 for(uint8_t n=0;n<18;n++)assert(telemetry_uart_enqueue_frame(0xa5,&n,1));
 assert(starts==1 && owned_len==5 && owned[4]==0);
 uint8_t x=99;assert(!telemetry_uart_enqueue_frame(0xa5,&x,1));
 assert(owned[4]==0 && g_gateway_uart_tx_pending==18);
 HAL_UART_TxCpltCallback(&other);assert(g_gateway_uart_tx_pending==18);
 for(unsigned n=0;n<18;n++){assert(owned[4]==n);complete();}
 assert(!owned && g_gateway_uart_tx_pending==0 && g_gateway_uart_tx_frames==18);
 assert(g_gateway_uart_tx_high_water==18 && g_gateway_uart_tx_queue_drops==1);
 start_status=HAL_ERROR;assert(telemetry_uart_enqueue_frame(0xa5,&x,1));
 telemetry_uart_flush_tx_queue();telemetry_uart_flush_tx_queue();telemetry_uart_flush_tx_queue();
 assert(g_gateway_uart_tx_exhausted==1 && g_gateway_uart_tx_retry_count==2);
 assert(!g_telemetry_uart.tx_count && aborts==3);
 start_status=HAL_OK;tick=0xfffffff0;assert(telemetry_uart_enqueue_frame(0xa5,&x,1));
 tick=60;telemetry_uart_flush_tx_queue();assert(owned && g_gateway_uart_tx_retry_count==3);
 complete();assert(g_gateway_uart_tx_frames==19);
 assert(!telemetry_uart_enqueue_frame(0xa5,&x,1025));
#ifndef SEDS_FIRMWARE_SIM_TEST
 uint8_t large[1024];memset(large,0x7c,sizeof large);
 assert(telemetry_uart_enqueue_frame(0xa5,large,sizeof large));
 assert(telemetry_uart_enqueue_frame(0xa5,large,sizeof large));
 assert(!telemetry_uart_enqueue_frame(0xa5,large,sizeof large));
 for(unsigned i=0;i<16;i++)assert(telemetry_uart_enqueue_frame(0xa5,&x,1));
 assert(owned_len==1028 && owned[1027]==0x7c);
 complete();assert(owned_len==1028 && owned[1027]==0x7c);
 complete();for(unsigned i=0;i<16;i++){assert(owned_len==5 && owned[4]==99);complete();}
 assert(!g_telemetry_uart.tx_buffer_used && !g_telemetry_uart.tx_count);
 // Repeated wraparound must free every descriptor and its physical buffer.
 for(unsigned i=0;i<1000;i++){assert(telemetry_uart_enqueue_frame(0xa5,&x,1));complete();}
 assert(!g_telemetry_uart.tx_buffer_used);
#endif
#ifdef SEDS_FIRMWARE_SIM_TEST
 assert(g_sim_uart_umbilical_status_count==19);
#endif
}
'''
  with tempfile.TemporaryDirectory() as d:
   p=Path(d); (p/'test.c').write_text(code)
   for flags in [[], ['-DSEDS_FIRMWARE_SIM_TEST=1']]:
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',*flags,str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
if __name__=='__main__':unittest.main()
