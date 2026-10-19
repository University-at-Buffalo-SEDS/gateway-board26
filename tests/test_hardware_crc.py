import subprocess
import tempfile
import unittest
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]

class HardwareCrcTests(unittest.TestCase):
    def test_incremental_state_fallback_and_interrupt_mask(self):
        source = (ROOT / "Core/Src/gateway_crc32.c").read_text()
        driver = source[source.index("int32_t gateway_crc32_update("):len(source)]
        harness = r"""
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
static uint32_t mask, calls, clocked, corrupt;
static volatile uint32_t g_gateway_crc_hw_state, g_gateway_crc_hw_calls;
static uint32_t __get_PRIMASK(void) { return mask; }
static void __disable_irq(void) { mask=1; }
static void __set_PRIMASK(uint32_t value) { mask=value; }
#define __HAL_RCC_CRC_CLK_ENABLE() (clocked++)
static uint32_t reference(uint32_t state, const uint8_t *bytes, size_t len) {
 for(size_t i=0;i<len;i++) {
  state ^= bytes[i];
  for(unsigned bit=0;bit<8;bit++) state=(state>>1)^(0xedb88320U & (0U-(state&1U)));
 }
 return state;
}
static uint32_t gateway_crc_hw_update(uint32_t state, const uint8_t *bytes, size_t len) {
 assert(mask && clocked); calls++;
 return reference(state,bytes,len)^corrupt;
}
"""
        main = r"""
int main(void) {
 uint8_t bytes[4097]; for(unsigned i=0;i<4097;i++) bytes[i]=(uint8_t)(i*73+i/251);
 for(unsigned initial_mask=0;initial_mask<2;initial_mask++) {
  for(unsigned len=0;len<=4096;len+=17) {
   mask=initial_mask;
   uint32_t state=UINT32_MAX, want=reference(state,bytes,len);
   assert(gateway_crc32_update(state,bytes,len/2,&state));
   assert(mask==initial_mask);
   assert(gateway_crc32_update(state,bytes+len/2,len-len/2,&state));
   assert(state==want && mask==initial_mask && g_gateway_crc_hw_state==1);
  }
 }
 uint32_t result=123, before=calls;
 assert(!gateway_crc32_update(0,bytes,4097,&result) && result==123 && calls==before);
 assert(!gateway_crc32_update(0,NULL,1,&result));
 assert(!gateway_crc32_update(0,bytes,1,NULL));
 // A failed peripheral check never overwrites the software hasher's state.
 g_gateway_crc_hw_state=0; corrupt=1; mask=1;
 assert(!gateway_crc32_update(UINT32_MAX,bytes,128,&result));
 assert(result==123 && mask==1 && g_gateway_crc_hw_state==2);
 before=calls; corrupt=0; mask=0;
 assert(!gateway_crc32_update(UINT32_MAX,bytes,128,&result));
 assert(result==123 && mask==0 && calls==before);
}
"""
        with tempfile.TemporaryDirectory() as tmp:
            exe = Path(tmp)/"crc"
            subprocess.run(["cc","-std=c11","-Wall","-Wextra","-Werror","-fsanitize=address,undefined","-x","c","-","-o",str(exe)],input=harness+driver+main,text=True,check=True)
            subprocess.run([str(exe)],check=True)

    def test_large_update_services_receive_fifo_and_survives_preemption(self):
        source = (ROOT / "Core/Src/gateway_crc32.c").read_text()
        driver = source[source.index("int32_t gateway_crc32_update("):]
        harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
static uint32_t mask,phase,queued,overflows,serviced,nested,nested_calls,max_chunk;
static volatile uint32_t g_gateway_crc_hw_state,g_gateway_crc_hw_calls;
int32_t gateway_crc32_update(uint32_t,const uint8_t*,size_t,uint32_t*);
static uint32_t __get_PRIMASK(void){return mask;}
static void __disable_irq(void){mask=1;}
static void __set_PRIMASK(uint32_t);
#define __HAL_RCC_CRC_CLK_ENABLE() ((void)0)
static uint32_t reference(uint32_t state,const uint8_t *p,size_t n){
 for(size_t i=0;i<n;i++){state^=p[i];for(unsigned bit=0;bit<8;bit++)state=(state>>1)^(0xedb88320U&(0U-(state&1U)));}return state;
}
static uint32_t gateway_crc_hw_update(uint32_t state,const uint8_t *p,size_t n){
 assert(mask);if(n>max_chunk)max_chunk=(uint32_t)n;
 // Three hardware receive slots; arrivals during a masked interval must
 // remain bounded. This is a work-budget regression, not a cycle calibration.
 phase+=(uint32_t)n;unsigned arrivals=phase/128;phase%=128;
 while(arrivals--){if(queued==3)overflows++;else queued++;}
 return reference(state,p,n);
}
static void __set_PRIMASK(uint32_t value){
 mask=value;
 if(mask)return;
 if(queued){serviced+=queued;queued=0;}
 if(!nested && nested_calls==0){
  nested=1;uint8_t other[23];for(unsigned i=0;i<23;i++)other[i]=(uint8_t)i;
  uint32_t result=0;assert(gateway_crc32_update(0x24681357U,other,23,&result));
  assert(result==reference(0x24681357U,other,23));nested_calls++;nested=0;
 }
}
''' + driver + r'''
int main(void){
 uint8_t bytes[4096];for(unsigned i=0;i<4096;i++)bytes[i]=(uint8_t)(i*73+i/251);
 uint32_t result=0;
 assert(gateway_crc32_update(UINT32_MAX,bytes,sizeof(bytes),&result));
 assert(result==reference(UINT32_MAX,bytes,sizeof(bytes)));
 assert(!mask && nested_calls==1 && serviced>0 && overflows==0);
 assert(max_chunk<=256);
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            exe = Path(tmp)/"preempt"
            subprocess.run(["cc","-std=c11","-Wall","-Wextra","-Werror","-fsanitize=address,undefined","-x","c","-","-o",str(exe)],input=harness,text=True,check=True)
            subprocess.run([str(exe)],check=True)
