"""An applied FLIGHT_STATE is acknowledged; durability is retried from the poll.

FLIGHT_STATE is the gateway's only Abort path. When the callback failed
after applying the value (storage not ready or the write failing) or
refused it because the OTA owner was busy, the SDK withheld the end-to-end
receipt: the station's ordered FLIGHT_STATE stream stalled behind that
write and newer writes, Aborted among them, queued behind it. Once applied,
the write is acknowledged and its persistence is retried from
flight_state_cache_poll at most once per second.

Builds the actual flight_state_cache.c with the test_cache_reset_owner
facade, plus a fault-injected OTA owner.
"""
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_cache_reset_owner import HARNESS as RESET_HARNESS, HEADERS as RESET_HEADERS

ROOT = Path(__file__).resolve().parents[1]

HEADERS = dict(RESET_HEADERS)
HEADERS["ota_stream.h"] = (
    "#pragma once\n#include <stdbool.h>\n#include <stdint.h>\n"
    "bool ota_stream_state_begin(uint8_t phase);\nvoid ota_stream_state_end(void);\n"
    "static inline void ota_stream_recovery_complete(uint32_t epoch){(void)epoch;}\n")

PRELUDE = RESET_HARNESS[:RESET_HARNESS.index("int main(int argc,char **argv){")]

CASES = r'''
static int ota_busy,ota_owned;
bool ota_stream_state_begin(uint8_t phase){assert(phase<=15U&&!ota_owned);if(ota_busy)return false;ota_owned=1;return true;}
void ota_stream_state_end(void){assert(ota_owned);ota_owned=0;}
extern volatile uint32_t g_flight_state_cache_errors;
static SedsRouter poll_router=1;
static void poll_at(unsigned at){tick=at;assert(flight_state_cache_poll(&poll_router)==0&&!ota_owned);}
static SedsResult apply(uint8_t s){SedsResult r=update(s);assert(!ota_owned);return r;}
int main(int argc,char **argv){
 assert(argc==2);SedsRouter router=0;
 assert(flight_state_cache_init(&router)==0&&flight_state_cache_value()==6);
 tick=10000;
 if(!strcmp(argv[1],"write_failure")){
  fail_write=1;
  /* Applied and acknowledged; durability is retried, not the receipt. */
  assert(apply(0)==SEDS_OK&&flight_state_cache_value()==0&&writes==1&&stored==6);
  poll_at(10999);assert(writes==1);                  /* at most once per second */
  poll_at(11000);assert(writes==2&&stored==6);
  fail_write=0;poll_at(11999);assert(writes==2);
  poll_at(12000);assert(writes==3&&stored==0);
  poll_at(30000);assert(writes==3);                  /* nothing pending */
  assert(apply(0)==SEDS_OK&&writes==3&&clears==0);   /* the applied cache is kept */
  assert(g_flight_state_cache_errors==2);
 }else if(!strcmp(argv[1],"ota_busy")){
  ota_busy=1;
  /* OTA owns flash: still applied and acknowledged, never written now. */
  assert(apply(15)==SEDS_OK&&flight_state_cache_value()==15&&writes==0);
  poll_at(11000);assert(writes==0);
  ota_busy=0;poll_at(12000);assert(writes==1&&stored==15);
 }else if(!strcmp(argv[1],"newer_value")){
  fail_write=1;assert(apply(1)==SEDS_OK&&apply(2)==SEDS_OK);
  fail_write=0;poll_at(11000);assert(stored==2);
 }else if(!strcmp(argv[1],"same_value_while_pending")){
  fail_write=1;assert(apply(13)==SEDS_OK);
  fail_write=0;assert(apply(13)==SEDS_OK&&stored==13);  /* a retry of the write persists */
  const unsigned w=writes;poll_at(20000);assert(writes==w);
 }else if(!strcmp(argv[1],"get_poll")){
  /* A seen value stops the GET; only the durability retry runs. */
  g_telemetry_discovery_seen=1;fail_write=1;assert(apply(1)==SEDS_OK);
  poll_at(20000);assert(requests==0);
 }else assert(!"unknown case");
 puts(argv[1]);return 0;
}
'''


class FlightStatePersistenceRetryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="gb-fstate-retry-")
        p = Path(cls.tmp.name)
        for name, source in HEADERS.items():
            (p / name).write_text(source)
        (p / "harness.c").write_text(PRELUDE + CASES)
        cls.exe = p / "test"
        subprocess.run(["cc", "-DTELEMETRY_ENABLED=1", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-Wno-unused-function", "-fsanitize=address,undefined", "-I", str(p),
                        "-I", str(ROOT / "Core/Inc"), str(ROOT / "Core/Src/flight_state_cache.c"),
                        str(ROOT / "Core/Src/board_recovery_query.c"), str(p / "harness.c"),
                        "-o", str(cls.exe)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_case(self, name):
        subprocess.run([str(self.exe), name], check=True)

    def test_failed_write_is_acknowledged_and_retried_once_per_second(self):
        self.run_case("write_failure")

    def test_busy_ota_owner_is_a_permit_not_a_refusal(self):
        self.run_case("ota_busy")

    def test_pending_retry_persists_the_latest_applied_value(self):
        self.run_case("newer_value")

    def test_identical_write_while_pending_persists_it(self):
        self.run_case("same_value_while_pending")

    def test_seen_value_still_stops_the_get(self):
        self.run_case("get_poll")


if __name__ == "__main__":
    unittest.main()
