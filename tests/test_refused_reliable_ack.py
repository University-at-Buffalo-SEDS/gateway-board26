"""Real SDK: a refused reliable FLIGHT_STATE is acknowledged, never applied.

The SDK withholds the end-to-end receipt (and aborts relay) when the managed
variable callback fails, so a deliberate refusal reported as an error made the
station retransmit the same packet forever. Storage and the recovery authority
decision are fixture inputs; this is not hardware or timing evidence.
"""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from native_sdk import prepare_native_sdk
from router_lifecycle_fixture import MOCK_HEADERS

ROOT = Path(__file__).resolve().parents[1]


class RefusedReliableAckTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.sdk = prepare_native_sdk(ROOT)
        cls.tmp = tempfile.TemporaryDirectory(prefix='refused-ack-')
        cls.path = Path(cls.tmp.name)
        for name, contents in MOCK_HEADERS.items():
            (cls.path / name).write_text(contents)
        cls.exe = cls.path / 'refused_ack'
        command = ['cc', '-std=c11', '-DTELEMETRY_ENABLED=1', '-Wall', '-Wextra', '-Werror', '-g',
                   '-fsanitize=address,undefined',
                   '-DCACHE_SOURCE="' + str(ROOT / 'Core/Src/flight_state_cache.c') + '"',
                   '-I' + str(cls.path), '-I' + str(ROOT / 'Core/Inc'),
                   '-I' + str(Path(cls.sdk['header']).parent),
                   str(ROOT / 'tests/native/refused_ack.in.c'), cls.sdk['archive'], '-lpthread', '-lm']
        if sys.platform.startswith('linux'):
            command.append('-ldl')
        command += ['-o', str(cls.exe)]
        subprocess.run(command, check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def case(self, mode):
        result = subprocess.run([str(self.exe), str(ROOT / 'config/sedsnet.json'), mode],
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('PASS ' + mode, result.stdout)

    def test_refused_station_write_is_acknowledged_and_not_applied(self):
        self.case('refused')

    def test_malformed_value_is_acknowledged_and_not_applied(self):
        self.case('malformed')

    def test_allowed_write_is_applied_and_acknowledged(self):
        self.case('allowed')

    def test_transient_storage_failure_stays_retryable(self):
        self.case('persist_fail')


if __name__ == '__main__':
    unittest.main()
