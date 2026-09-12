"""The tracked translation matches the loader's pinned exe digest and is complete."""
import json
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
TRANSLATION = ROOT / "translation"


class TrackedTranslation(unittest.TestCase):
    def test_digest_matches_loader(self):
        loader = (ROOT / "src/recomp/runtime/loader.cpp").read_text()
        pinned = re.search(r'LOADER_EXPECTED_SHA256 =\s*"([0-9a-f]{64})"', loader).group(1)
        symbols = json.loads((TRANSLATION / "symbols.json").read_text())
        self.assertEqual(symbols["exe_sha256"], pinned)

    def test_every_chunk_present(self):
        names = sorted(p.name for p in TRANSLATION.glob("chunk_*.c"))
        self.assertGreater(len(names), 0)
        self.assertTrue((TRANSLATION / "table.c").is_file())
        self.assertTrue((TRANSLATION / "x86.h").is_file())
        self.assertTrue((TRANSLATION / "funcs.h").is_file())
        # Contiguous numbering: a missing chunk would leave a hole.
        numbers = [int(n[6:9]) for n in names]
        self.assertEqual(numbers, list(range(len(numbers))))


if __name__ == "__main__":
    unittest.main()
