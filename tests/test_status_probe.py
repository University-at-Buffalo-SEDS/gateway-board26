"""Compile passive status diagnostics against full and compact frames."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]

class StatusProbeTests(unittest.TestCase):
    def test_status_path_classification(self):
        code = r"""
#include <assert.h>
#include "gateway_status_probe.h"
volatile gateway_status_probe g_gateway_status_path[4];
gateway_probe_template g_gateway_status_templates[2][GATEWAY_STATUS_TEMPLATE_CAPACITY];
uint32_t g_gateway_status_next[2];
static size_t put(uint8_t *p, uint32_t v) {
  size_t n=0; do { p[n++]=(v&127)|(v>127?128:0); v>>=7; } while(v); return n;
}
int main(void) {
  uint8_t p[64]={83,68,84,1,7,0,1,42,2,1};
  size_t n=10; n+=put(p+n,3852079518U);
  gateway_status_observe(0,p,n,42,100);
  assert(g_gateway_status_path[0].valve_status==1);
  gateway_status_observe(1,p,n,42,101);
  gateway_status_observe(2,p,n,42,102);
  uint8_t compact[]={83,68,84,2,0x48,7};
  for(unsigned kind=2;kind<=5;kind++) {
    if(kind==3) continue;
    compact[3]=kind;
    gateway_status_observe(1,compact,sizeof compact,42,103);
    gateway_status_observe(2,compact,sizeof compact,42,104);
  }
  assert(g_gateway_status_path[1].valve_status==4);
  assert(g_gateway_status_path[2].valve_status==4);
  compact[5]=8;
  gateway_status_observe(2,compact,sizeof compact,42,105);
  assert(g_gateway_status_path[2].unknown==1);
  assert(g_gateway_status_path[2].last_tick==104);
  gateway_status_observe(0,p+5,n-5,42,106);
  assert(g_gateway_status_path[0].valve_status==2);
  for(size_t i=0;i<n;i++) gateway_status_observe(3,p,i,42,0);
  assert(g_gateway_status_path[3].status==0);
  gateway_status_observe(3,p,n,42,107);
  assert(g_gateway_status_path[3].valve_status==1);
  n=10; n+=put(p+n,483403151U);
  gateway_status_observe(0,p,n,42,108);
  assert(g_gateway_status_path[0].actuator_status==1);
  gateway_status_observe(4,0,0,42,0);
  assert(sizeof(g_gateway_status_path)+sizeof(g_gateway_status_templates)+sizeof(g_gateway_status_next)==296);
}
"""
        with tempfile.TemporaryDirectory() as tmp:
            exe = str(pathlib.Path(tmp) / "probe")
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-I", str(ROOT / "Core/Inc"), "-x", "c", "-", "-o", exe],
                           input=code, text=True, check=True)
            subprocess.run([exe], check=True)
