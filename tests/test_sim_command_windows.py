"""Command progress must distinguish restart discovery from a lost command."""
import importlib.util
from pathlib import Path
import unittest
ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("run_full", ROOT / "sim/run_full.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

class CommandProgressTests(unittest.TestCase):
    def test_normal_intervals_stay_strict(self):
        for start in range(11):
            self.assertEqual(module.command_progress_end_sample(start, 12, set()), start + 1)
    def test_only_the_post_restart_window_is_extended(self):
        for start in range(11):
            expected = start + (2 if start in (3, 7) else 1)
            self.assertEqual(module.command_progress_end_sample(start, 12, {4, 8}), expected)
    def test_captured_recovery_does_not_report_an_unsent_command_as_lost(self):
        counts = [1,2,3,3,5,6,7,8,8,11,12,13]
        self.assertEqual(counts[8] - counts[7], 0)
        end = module.command_progress_end_sample(7, 12, {4,8})
        self.assertGreaterEqual(counts[end] - counts[7], 1)
        counts[9] = counts[7]
        self.assertEqual(counts[end] - counts[7], 0, "a genuine post-recovery stall must still fail")
    def test_end_never_exceeds_the_last_snapshot(self):
        self.assertEqual(module.command_progress_end_sample(9, 11, {10}), 10)
        with self.assertRaises(ValueError):
            module.command_progress_end_sample(10, 11, {10})
if __name__ == "__main__":
    unittest.main()
