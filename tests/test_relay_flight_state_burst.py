"""Real SDK: a burst of station FLIGHT_STATE writes relayed onto the fill CAN.

The gateway acknowledges each write to the station when it accepts it and owns
the CAN delivery. One discovered CAN owner stops answering (AB); every write
must still reach VB and DAQ in order, 1 s and 100 ms apart. SEDSnet 7.0.0
dropped the writes queued behind each exhausted one at the gateway: VB and DAQ
applied 2 of 8 writes 1 s apart and 1 of 8 100 ms apart (firmware sim: GB
applied every write, VB and DAQ stayed in Fill). The
fix is in the SDK (segment proxy ordered tail); with an SDK that lacks it the
cases are skipped, naming the missing fix. Transport is ideal and in-process.
"""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from native_sdk import prepare_native_sdk
from router_lifecycle_fixture import MOCK_HEADERS

ROOT = Path(__file__).resolve().parents[1]
# The SDK change that keeps a segment proxy's ordered tail (SEDSnet dev, after 7.0.0).
FIX_MARKER = 'fn retire_ordered_gap_owners_locked'


class RelayFlightStateBurstTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.sdk = prepare_native_sdk(ROOT)
        router = Path(cls.sdk['source']) / 'src/router.rs'
        cls.sdk_fixed = router.is_file() and FIX_MARKER in router.read_text()
        cls.tmp = tempfile.TemporaryDirectory(prefix='relay-burst-')
        cls.path = Path(cls.tmp.name)
        for name, contents in MOCK_HEADERS.items():
            (cls.path / name).write_text(contents)
        cls.exe = cls.path / 'relay_burst'
        command = ['cc', '-std=c11', '-DTELEMETRY_ENABLED=1', '-Wall', '-Wextra', '-Werror', '-g',
                   '-fsanitize=address,undefined',
                   '-DCACHE_SOURCE="' + str(ROOT / 'Core/Src/flight_state_cache.c') + '"',
                   '-I' + str(cls.path), '-I' + str(ROOT / 'Core/Inc'),
                   '-I' + str(Path(cls.sdk['header']).parent),
                   str(ROOT / 'tests/native/relay_burst.in.c'), cls.sdk['archive'], '-lpthread', '-lm']
        if sys.platform.startswith('linux'):
            command.append('-ldl')
        command += ['-o', str(cls.exe)]
        subprocess.run(command, check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def burst(self, spacing_ms):
        result = subprocess.run([str(self.exe), str(ROOT / 'config/sedsnet.json'), str(spacing_ms)],
                                capture_output=True, text=True, timeout=120)
        if not self.sdk_fixed:
            self.skipTest('the SDK lacks the segment-proxy ordered tail fix (SEDSnet after 7.0.0): '
                          + result.stdout.strip().replace('\n', '; '))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(f'PASS {spacing_ms}', result.stdout)

    def test_writes_one_second_apart_reach_vb_and_daq(self):
        self.burst(1000)

    def test_writes_100_ms_apart_reach_vb_and_daq(self):
        self.burst(100)


if __name__ == '__main__':
    unittest.main()
