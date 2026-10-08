import re
import unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
class CurrentPolarity(unittest.TestCase):
    def test_schematic_upstream_v1_is_positive_draw(self):
        header=(ROOT/'Core/Inc/ltc2990.h').read_text()
        value=re.search(r'#define CURRENT_DRAW_POLARITY\s+\(([-0-9.]+)f\)',header)
        self.assertEqual(float(value.group(1)),1.0)
