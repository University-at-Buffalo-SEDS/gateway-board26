"""The simulator-only failure snapshot holds every probe the simulator reads."""
import json
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = (ROOT / "Core/Inc/sim_failure_snapshot.h").read_text()


def define(name):
    return int(re.search(rf"#define {name} (\w+?)U?\n", HEADER).group(1), 0)


class SimFailureSnapshotTests(unittest.TestCase):
    def test_board_layout_probes_fit_the_snapshot(self):
        words = define("SIM_FAILURE_SNAPSHOT_WORDS")
        layout = json.loads((ROOT / "sim/board.json").read_text())
        offsets = [
            probe.get("byte_offset", 0)
            for probe in layout["execution"]["memory_probes"]
            if probe["symbol"] == "g_telemetry_alloc_failure_stack"
        ]
        self.assertTrue(offsets)
        self.assertLessEqual(max(offsets) + 4, 4 * words)

    def test_router_counters_and_can_drop_frame_do_not_overlap(self):
        words = define("SIM_FAILURE_SNAPSHOT_WORDS")
        frame = define("SIM_FAILURE_SNAPSHOT_CAN_FRAME_FIRST")
        telemetry = (ROOT / "Core/Src/telemetry.c").read_text()
        queue_first = int(re.search(
            r"#define g_gateway_queue_counters \(g_telemetry_alloc_failure_stack \+ (\d+)U\)",
            telemetry).group(1))
        memory_used = max(int(i) for i in re.findall(
            r"g_gateway_memory_counters\[(\d+)\]", telemetry)) + 1
        self.assertLessEqual(memory_used, queue_first)
        # 16 queue counters, then the CAN drop PSP and tag at frame - 2.
        self.assertLessEqual(queue_first + 16, frame - 2)
        self.assertLess(frame, words)

    def test_flight_images_do_not_carry_the_snapshot(self):
        self.assertRegex(HEADER, r"#ifdef SEDS_FIRMWARE_SIM_TEST\n[\s\S]*SIM_FAILURE_SNAPSHOT_WORDS[\s\S]*#endif")


if __name__ == "__main__":
    unittest.main()
