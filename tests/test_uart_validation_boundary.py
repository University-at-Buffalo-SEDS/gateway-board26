from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class UartValidationBoundary(unittest.TestCase):
    def test_framing_delegates_one_validation_without_a_temporary_packet(self):
        source = (ROOT / "Core/Src/telemetry_uart.c").read_text()
        start = source.index("static void telemetry_uart_dispatch_data_payload(const uint8_t *payload, size_t payload_len) {")
        end = source.index("\nstatic void ", start + 1)
        code = r'''
#include <stdint.h>
#include <stddef.h>
#include <assert.h>
#define TELEMETRY_UART_REQ_DATA_MAGIC 0xa5
static struct { unsigned nested_fill, nested_discard_remaining; } g_telemetry_uart;
static unsigned validations, deliveries, dispatches, nested_bytes;
int seds_pkt_validate_packed(const uint8_t *p, size_t n) {
    validations++; return n && (p[0]==0x2c || p[0]=='S') ? 0 : -1;
}
static uint8_t telemetry_uart_is_valid_header(uint8_t a,uint8_t b) { return a==0xa5 && b==0x5a; }
static void telemetry_uart_dispatch_frame(uint8_t magic,const uint8_t *p,size_t n) {
    assert(magic==0xa5);dispatches++;
    // Stand in for the central side-aware receiver: a rejected envelope never
    // reaches an application handler. Framing itself must not unpack a copy.
    if(seds_pkt_validate_packed(p,n)==0) deliveries++;
}
static void telemetry_uart_nested_rx_push_byte(uint8_t b) {(void)b;nested_bytes++;}
void telemetry_uart_signal_parse_failure(void) {}
''' + source[start:end] + r'''
int main(void) {
    const uint8_t canonical[]={0x2c,1,109,1,7};
    telemetry_uart_dispatch_data_payload(canonical,sizeof canonical);
    assert(dispatches==1 && validations==1 && deliveries==1);
    const uint8_t invalid[]={0,1,109};
    telemetry_uart_dispatch_data_payload(invalid,sizeof invalid);
    assert(dispatches==2 && validations==2 && deliveries==1);
    const uint8_t wrapped[]={'S','D','T',1,42};
    telemetry_uart_dispatch_data_payload(wrapped,sizeof wrapped);
    assert(dispatches==3 && validations==3 && deliveries==2);
    const uint8_t nested[]={0xa5,0x5a,1,0,0};
    telemetry_uart_dispatch_data_payload(nested,sizeof nested);
    assert(nested_bytes==sizeof nested && dispatches==3 && validations==3);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / "test.c").write_text(code)
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", str(path / "test.c"), "-o", str(path / "test")], check=True)
            subprocess.run([str(path / "test")], check=True)


if __name__ == "__main__":
    unittest.main()
