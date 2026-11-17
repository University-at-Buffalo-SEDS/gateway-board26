"""The router owner reports SEDSnet network time and never restarts it.

SEDSnet 6 steps a follower to its source's newer time-base epoch by itself,
so the board no longer compares its network clock with a station clock built
from GS stamps or restarts time sync when they disagree. The actual
telemetry_update_network_health() runs with modeled SDK calls.
"""
from pathlib import Path
import re,subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
def compile_and_run(test,code):
 with tempfile.TemporaryDirectory() as d:
  exe=Path(d)/'probe';out=subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-x','c','-','-o',str(exe)],input=code,text=True,capture_output=True)
  test.assertEqual(out.returncode,0,out.stderr);subprocess.run([str(exe)],check=True,timeout=10)

class NetworkClockPublication(unittest.TestCase):
 def test_network_time_is_reported_without_a_station_clock(self):
  source=(ROOT/'Core/Src/telemetry.c').read_text()
  for removed in ('board_recovery_network_clock_resync_needed','board_recovery_observe_station_time','g_network_clock_resyncs'):
   self.assertNotIn(removed,source)
  body=re.search(r'static void telemetry_update_network_health\(SedsRouter \*router\)\s*\{.*?\n\}',source,re.S).group()
  code=r'''#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
typedef int SedsResult;typedef int SedsRouter;
#define SEDS_OK 0
static SedsRouter router;static bool valid;static unsigned reads;
static uint32_t g_telemetry_timesync_valid,g_telemetry_discovery_seen=1,g_telemetry_network_ready;
static SedsResult seds_router_get_network_time_ms(SedsRouter*r,uint64_t*out){assert(r==&router);reads++;if(!valid)return -1;*out=1794183600000ULL;return SEDS_OK;}
'''+body+r'''
int main(void){
 valid=false;telemetry_update_network_health(&router);assert(reads==1&&!g_telemetry_timesync_valid&&!g_telemetry_network_ready);
 valid=true;telemetry_update_network_health(&router);assert(reads==2&&g_telemetry_timesync_valid&&g_telemetry_network_ready);
 return 0;}
'''
  compile_and_run(self,code)
if __name__=='__main__':unittest.main()
