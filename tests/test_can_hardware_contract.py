import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

class CanHardwareContract(unittest.TestCase):
    def test_preserves_bitrate_with_later_sample_point(self):
        source = (ROOT / "Core/Src/main.c").read_text()
        ioc = (ROOT / "gateway_board.ioc").read_text()
        self.assertIn("hfdcan2.Init.AutoRetransmission = ENABLE", source)
        self.assertIn("hfdcan2.Init.NominalPrescaler = 2", source)
        self.assertIn("hfdcan2.Init.NominalTimeSeg1 = 19", source)
        self.assertIn("hfdcan2.Init.NominalTimeSeg2 = 4", source)
        self.assertIn("FDCAN2.CalculateBaudRateNominal=3541666", ioc)
        self.assertIn("FDCAN2.AutoRetransmission=ENABLE", ioc)
        self.assertEqual(2 * (1 + 19 + 4), 16 * (1 + 1 + 1))
        self.assertAlmostEqual((1 + 19) / (1 + 19 + 4), 5 / 6)
        self.assertIn("hfdcan2.Init.NominalSyncJumpWidth = 4", source)
        can = (ROOT / "Core/Src/can_bus.c").read_text()
        self.assertIn("can_tx_queue_submit", can)
        self.assertNotIn("can_bus_wait_for_tx_slot", can)
        self.assertNotIn("< (uint32_t)frag_cnt", can)
