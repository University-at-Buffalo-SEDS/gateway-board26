#include "uart_integrity.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
@CONSTANTS@
@STATS@
typedef int SedsResult;
typedef struct { unsigned unused; } UART_HandleTypeDef;
#define SEDS_BAD_ARG (-1)
#define SEDS_OK 0
#define GREEN_LED_GPIO_Port 1
#define GREEN_LED_Pin 2
#define GPIO_PIN_RESET 0
static struct {
 UART_HandleTypeDef *huart;
 uint8_t rx_ring[TELEMETRY_UART_RX_RING_BYTES],rx_frame[TELEMETRY_UART_FRAME_SIZE];
 unsigned rx_head,rx_tail,rx_count,rx_dma_drop_count,rx_dma_active;
 size_t rx_fill,rx_expected,rx_discard_remaining;
 int side_id; TelemetryUartStats stats;
} g_telemetry_uart;
static UartIntegrityReader g_integrity_reader;
static uint8_t g_sedsnet_integrity;
static uint32_t g_gateway_uart_rx_frames,g_gateway_uart_rx_ring_drops;
static uint32_t tick,advance_crc,advance_emit,flushes,restarts,opaque_bytes,mask;
static unsigned hardware_crc;
static uint32_t telemetry_uart_irq_save(void){uint32_t old=mask;mask=1;return old;}
static void telemetry_uart_irq_restore(uint32_t n){mask=n;}
static uint32_t HAL_GetTick(void){return tick;}
static void HAL_GPIO_WritePin(unsigned a,unsigned b,unsigned c){assert(a==1&&b==2&&c==0);}
static void telemetry_uart_start_rx_dma(void){++restarts;g_telemetry_uart.rx_dma_active=1;}
static void telemetry_uart_flush_tx_queue(void){++flushes;}
static void telemetry_uart_process_rx_byte(uint8_t byte){(void)byte;++opaque_bytes;}
static uint32_t reference(uint32_t state,const uint8_t *p,size_t n){for(size_t i=0;i<n;++i){state^=p[i];for(unsigned b=0;b<8;++b)state=(state>>1)^(0xedb88320U&(0U-(state&1U)));}return state;}
static int gateway_crc32_update(uint32_t state,const uint8_t *p,size_t n,uint32_t *out){tick+=advance_crc;if(!hardware_crc)return 0;*out=reference(state,p,n);return 1;}
static unsigned calls,commands,ascii;
static uint8_t captured[8192];static size_t captured_len;
static void telemetry_uart_dispatch_frame(uint8_t kind,const uint8_t *p,size_t n){
 assert(!mask&&n<=1024);++calls;if(kind==0xa6)++commands;else if(kind==0xa7)++ascii;else assert(kind==0xa5);
 assert(captured_len+3+n<=sizeof(captured));captured[captured_len++]=kind;captured[captured_len++]=(uint8_t)n;captured[captured_len++]=(uint8_t)(n>>8);memcpy(captured+captured_len,p,n);captured_len+=n;tick+=advance_emit;
}
@FUNCTIONS@
static void reset(unsigned strict){
 tick=advance_crc=advance_emit=flushes=restarts=opaque_bytes=mask=0;g_gateway_uart_rx_frames=g_gateway_uart_rx_ring_drops=0;calls=commands=ascii=0;captured_len=0;hardware_crc=0;
 static UART_HandleTypeDef uart;assert(telemetry_uart_init(&uart)==0);assert(!g_sedsnet_integrity);if(strict)telemetry_uart_set_sedsnet_integrity();
}
static size_t body(uint8_t *out,size_t n,uint8_t seed){assert(n>=8&&n<=1024);for(size_t i=0;i<n-4;++i)out[i]=(uint8_t)(seed+i*73+i/251);uint32_t crc=~reference(UINT32_MAX,out,n-4);for(unsigned i=0;i<4;++i)out[n-4+i]=(uint8_t)(crc>>(8*i));return n;}
static size_t frame(uint8_t *out,uint8_t k,const uint8_t *p,size_t n){out[0]=k;out[1]=k==0xa5?0x5a:k==0xa6?0x5b:0x7a;out[2]=(uint8_t)n;out[3]=(uint8_t)(n>>8);memcpy(out+4,p,n);return n+4;}
static void feed(const uint8_t*p,size_t n){while(n){size_t take=n>512?512:n;telemetry_uart_rx_ring_push_isr(p,(uint16_t)take);p+=take;n-=take;}}
static void drain(void){for(unsigned i=0;i<10000;++i){size_t before=g_telemetry_uart.rx_count;telemetry_uart_process();assert(!mask);if(!g_telemetry_uart.rx_count&&!before&&(!g_sedsnet_integrity||g_integrity_reader.needs_input))break;}}
int main(int argc,char**argv){assert(argc==2);unsigned c=(unsigned)atoi(argv[1]);uint8_t b[1024],wire[8192],other[1024];
 if(c==0||c==1||c==6){
  reset(1);body(b,13,1);size_t n=frame(wire,0xa5,b,13);frame(wire+n,0xa5,b,13);feed(wire,n*2);if(c==1)advance_crc=1;else advance_emit=1;if(c==6)tick=UINT32_MAX;
  telemetry_uart_process();printf("deadline calls=%u tick=%u remaining=%u\n",calls,tick,g_telemetry_uart.rx_count);fflush(stdout);assert(calls==1&&tick==(c==6?0:1));advance_emit=advance_crc=0;drain();assert(calls==2);
 }else if(c==2){body(b,1024,11);size_t n=frame(wire,0xa5,b,1024);for(size_t cut=0;cut<n;++cut){reset(1);feed(wire,cut);drain();assert(!calls);feed(wire+cut,n-cut);drain();assert(calls==1&&captured_len==1027&&!memcmp(captured+3,b,1024));}reset(1);for(size_t i=0;i<n;++i){feed(wire+i,1);telemetry_uart_process();}drain();assert(calls==1);
 }else if(c==3){reset(1);body(b,508,23);const uint8_t hidden[]={0xa6,0x5b,6,0,'r','e','s','e','t','\n',0xa7,0x7a,1,0,'x'};memcpy(b+64,hidden,sizeof hidden);body(other,64,55);frame(b+256,0xa5,other,64);uint32_t crc=~reference(UINT32_MAX,b,504);for(unsigned i=0;i<4;++i)b[504+i]=(uint8_t)(crc>>(8*i));size_t n=frame(wire,0xa5,b,508);feed(wire,n);drain();assert(calls==1&&!commands&&!ascii&&!memcmp(captured+3,b,508));
 }else if(c==4){reset(1);memset(b,0x11,40);const uint8_t hidden[]={0xa6,0x5b,6,0,'r','e','s','e','t','\n',0xa7,0x7a,1,0,'x'};memcpy(b+4,hidden,sizeof hidden);size_t n=frame(wire,0xa5,b,40);n+=frame(wire+n,0xa6,(const uint8_t*)"diagnostic",10);body(other,64,17);n+=frame(wire+n,0xa5,other,64);n+=frame(wire+n,0xa6,(const uint8_t*)"status",6);n+=frame(wire+n,0xa7,(const uint8_t*)"show",4);feed(wire,n);drain();assert(calls==3&&commands==1&&ascii==1&&g_integrity_reader.quarantined_commands>=3);
 }else if(c==5){reset(1);body(b,508,13);size_t n=frame(wire,0xa5,b,508);for(unsigned i=1;i<8;++i)memcpy(wire+i*n,wire,n);feed(wire,4096);feed(wire,1);assert(g_gateway_uart_rx_ring_drops==1);for(unsigned i=0;i<100;++i){uint32_t before=g_telemetry_uart.stats.rx_byte_count;telemetry_uart_process();assert(g_telemetry_uart.stats.rx_byte_count-before<=512);if(!g_telemetry_uart.rx_count&&calls==8)break;}assert(calls==8);feed(wire,512);drain();assert(calls==9);
 }else if(c==7){reset(0);feed((const uint8_t*)"notCRC",6);telemetry_uart_process();assert(opaque_bytes==6&&!calls);telemetry_uart_set_sedsnet_integrity();body(b,64,3);size_t n=frame(wire,0xa5,b,64);feed(wire,n);telemetry_uart_process();assert(calls==1);telemetry_uart_set_sedsnet_integrity();assert(calls==1);
 }else if(c==8){reset(1);body(b,1024,7);for(unsigned hw=0;hw<2;++hw){hardware_crc=hw;assert(telemetry_uart_data_crc(b,1024));b[1023]^=1;assert(!telemetry_uart_data_crc(b,1024));b[1023]^=1;}assert(!telemetry_uart_data_crc(b,7));
 }else if(c==9){ // Budget pause never consumes a candidate or clears taint.
  reset(1);memset(b,0x11,24);size_t n=frame(wire,0xa5,b,24);body(other,64,19);n+=frame(wire+n,0xa5,other,64);
  for(size_t i=0;i<n;++i)assert(uart_integrity_push(&g_integrity_reader,wire[i]));
  UartIntegrityBudget small={1,4096};assert(uart_integrity_drain(&g_integrity_reader,&small,telemetry_uart_data_crc,telemetry_uart_integrity_emit)==UART_INTEGRITY_BUDGET);assert(g_integrity_reader.tainted&&!calls);
  size_t used=g_integrity_reader.used,cursor=g_integrity_reader.cursor;uint8_t saved[1028];memcpy(saved,g_integrity_reader.bytes,used);UartIntegrityBudget none={0,4096};assert(uart_integrity_drain(&g_integrity_reader,&none,telemetry_uart_data_crc,telemetry_uart_integrity_emit)==UART_INTEGRITY_BUDGET);assert(used==g_integrity_reader.used&&cursor==g_integrity_reader.cursor&&g_integrity_reader.tainted&&!memcmp(saved,g_integrity_reader.bytes,used));
  drain();assert(calls==1&&!memcmp(captured+3,other,64));UartIntegrityReader snapshot=g_integrity_reader;
  for(unsigned i=0;i<10;++i){UartIntegrityBudget budget={64,4096};assert(uart_integrity_drain(&g_integrity_reader,&budget,telemetry_uart_data_crc,telemetry_uart_integrity_emit)==UART_INTEGRITY_NEEDS_INPUT);assert(!memcmp(&snapshot,&g_integrity_reader,sizeof snapshot));}
 }else if(c==10){ // Losing a Data magic byte cannot authorize its embedded command.
  reset(1);body(b,128,7);const uint8_t cmd[]={0xa6,0x5b,6,0,'r','e','s','e','t','\n'};memcpy(b+16,cmd,sizeof cmd);uint32_t crc=~reference(UINT32_MAX,b,124);for(unsigned i=0;i<4;++i)b[124+i]=(uint8_t)(crc>>(8*i));size_t n=frame(wire,0xa5,b,128);wire[0]^=1;body(other,64,8);n+=frame(wire+n,0xa5,other,64);feed(wire,n);drain();printf("headercorruption commands=%u calls=%u\n",commands,calls);fflush(stdout);assert(!commands&&calls==1);
 }else if(c==11){ // Finite deletions/corruptions recover every fresh suffix byte.
  const unsigned offsets[]={0,1,2,3,4,5,17,63,127,255,507,511};
  for(unsigned mode=0;mode<2;++mode)for(unsigned j=0;j<sizeof offsets/sizeof offsets[0];++j){reset(1);body(b,508,13);size_t n=frame(wire,0xa5,b,508);unsigned at=offsets[j];if(mode){wire[at]^=0x80;}else{memmove(wire+at,wire+at+1,--n-at);}body(other,64,99);size_t fresh=n;for(unsigned k=0;k<3;++k)n+=frame(wire+n,0xa5,other,64);feed(wire,n);drain();assert(!commands&&!ascii&&calls==3);for(unsigned k=0;k<3;++k){assert(captured[k*67]==0xa5&&!memcmp(captured+k*67+3,wire+fresh+4,64));}}
 }else assert(0);puts("PASS");return 0;
}
