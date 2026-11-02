from pathlib import Path
import re
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
def function(source,signature):
 match=re.search(r'^'+re.escape(signature)+r'[^;{}]*\{',source,re.M);assert match,signature
 start=match.start();pos=source.index('{',start);depth=1;end=pos+1
 while depth:
  depth+=(source[end]=='{')-(source[end]=='}');end+=1
 return source[start:end]
def fixture():
 s=(ROOT/'Core/Src/telemetry_uart.c').read_text();h=(ROOT/'Core/Inc/telemetry_uart.h').read_text()
 names=['static void telemetry_uart_rx_ring_push_isr(', 'static uint8_t telemetry_uart_rx_ring_pop_byte(', 'static void telemetry_uart_reset_rx(', 'SedsResult telemetry_uart_init(', 'static bool telemetry_uart_data_crc(', 'static void telemetry_uart_integrity_emit(', 'void telemetry_uart_set_sedsnet_integrity(']
 for name in ['static bool telemetry_uart_strict_time_remaining(', 'static bool telemetry_uart_service_strict_rx(']:
  if name in s:names.append(name)
 names.append('void telemetry_uart_process(')
 macros=[]
 for text in[h,s]:
  lines=iter(text.splitlines())
  for line in lines:
   if not line.startswith('#define TELEMETRY_UART_'):continue
   while line.endswith('\\'):line+='\n'+next(lines)
   name=line.split()[1]
   if name not in {x.split()[1] for x in macros}:macros.append(line)
 stats=re.search(r'typedef struct \{.*?\} TelemetryUartStats;',h,re.S).group()
 return(ROOT/'tests/native/uart_integrity.in.c').read_text().replace('@CONSTANTS@','\n'.join(macros)).replace('@STATS@',stats).replace('@FUNCTIONS@','\n'.join(function(s,n)for n in names))
class UartIntegrityTests(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  cls.tmp=tempfile.TemporaryDirectory(prefix='gateway-uart-integrity-');p=Path(cls.tmp.name);cls.exe=p/'test';(p/'test.c').write_text(fixture())
  subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-I'+str(ROOT/'Core/Inc'),str(p/'test.c'),str(ROOT/'Core/Src/uart_integrity.c'),'-o',str(cls.exe)],check=True)
 @classmethod
 def tearDownClass(cls):cls.tmp.cleanup()
 def check(self,c):subprocess.run([str(self.exe),str(c)],check=True)
 def test_callback_deadline(self):self.check(0)
 def test_crc_deadline(self):self.check(1)
 def test_every_split_and_onebyte_fragments(self):self.check(2)
 def test_valid_parent_atomic(self):self.check(3)
 def test_command_and_ascii_quarantine(self):self.check(4)
 def test_actual_ring_budget_wrap_and_refusal(self):self.check(5)
 def test_deadline_wrap(self):self.check(6)
 def test_explicit_profile_only(self):self.check(7)
 def test_hardware_crc_and_fallback(self):self.check(8)
 def test_budget_pause_retains_work_and_noinput_is_inert(self):self.check(9)
 def test_corrupt_header_cannot_authorize_embedded_command(self):self.check(10)
 def test_deterministic_deletion_and_corruption_suffix(self):self.check(11)
if __name__=='__main__':unittest.main()
