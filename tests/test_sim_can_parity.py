"""Simulator observability must not replace production CAN reception."""
from pathlib import Path
import subprocess
import unittest
ROOT = Path(__file__).resolve().parents[1]
class CanBuildParityTests(unittest.TestCase):
    def test_can_interrupt_default_and_explicit_override(self):
        source = (ROOT / "Core/Src/can_bus.c").read_text()
        start = source.index("#ifndef CAN_BUS_POLLING")
        end = source.index("\n\n", start)
        block = source[start:end]
        for flags, expected in [([], "0"), (["-DSEDS_FIRMWARE_SIM_TEST=1"], "0"),
                                (["-DCAN_BUS_POLLING=1"], "1")]:
            result = subprocess.run(["cc", "-E", "-P", "-x", "c", *flags, "-"],
                input=block+"\nCAN_BUS_POLLING\n", text=True, capture_output=True, check=True)
            self.assertEqual(result.stdout.strip(), expected)
if __name__ == "__main__":
    unittest.main()
