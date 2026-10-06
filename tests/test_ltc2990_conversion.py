import re
import subprocess
import tempfile
import unittest
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
class CurrentConversionTests(unittest.TestCase):
    def test_actual_converter_signed_boundaries_and_shunt_scaling(self):
        header = (ROOT / "Core/Inc/ltc2990.h").read_text()
        source = (ROOT / "Core/Src/ltc2990.c").read_text()
        function = re.search(r"float LTC2990_Code15_To_CurrentA\(uint16_t raw15\)\s*\{.*?\n\}", source, re.S).group()
        # Resolve the production shunt/divider macros, including continuations.
        logical = header.replace("\\\n", " ")
        macros = "\n".join(line for line in logical.splitlines() if line.startswith("#define ") and line.split()[1] in {"RSENSE_OHM", "CURRENT_DIVIDER_RATIO", "CURRENT_DIVIDER_TOP_OHM", "CURRENT_DIVIDER_BOTTOM_OHM"})
        expected = 0.00791365
        program = "#include <stdint.h>\n#include <assert.h>\n#include <math.h>\n" + macros + "\n" + function + f"\nint main(void){{const float step={expected};" + """
        const uint16_t inputs[]={0,1,0x3fff,0x4000,0x7ffe,0x7fff,0xffff,0x8001};
        const int counts[]={0,1,16383,-16384,-2,-1,-1,1};
        for(unsigned i=0;i<8;i++){
          float want=counts[i]*step;
          assert(fabsf(LTC2990_Code15_To_CurrentA(inputs[i])-want)<fabsf(want)*1e-6f+1e-8f);
        }
        }
        """
        with tempfile.TemporaryDirectory() as tmp:
            p=Path(tmp); (p/"test.c").write_text(program)
            subprocess.run(["cc","-std=c11","-fsanitize=address,undefined",str(p/"test.c"),"-lm","-o",str(p/"test")],check=True)
            subprocess.run([str(p/"test")],check=True)
