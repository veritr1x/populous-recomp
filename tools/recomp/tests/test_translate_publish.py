"""Publishing a translation: a failure leaves the published tree alone; a success swaps it whole."""

import importlib.util
from pathlib import Path
import shutil
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("build_py", Path(__file__).parents[3] / "tools/build.py")
build_py = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build_py)


class PublishTests(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp(prefix="publish-"))
        self.addCleanup(shutil.rmtree, self.root, True)
        (self.root / "tools/recomp/runtime").mkdir(parents=True)
        (self.root / "tools/recomp/runtime/x86.h").write_text("// x86\n")
        self.gen = self.root / "build/recomp/gen"
        self.gen.mkdir(parents=True)
        (self.gen / "table.c").write_text("old\n")
        (self.gen / "symbols.json").write_text('{"generation": 1}\n')
        (self.root / "build/recomp/symbols.json").write_text('{"generation": 1}\n')

    def test_failed_translation_leaves_the_published_tree_untouched(self):
        def failing(stage):
            (stage / "table.c").write_text("half\n")
            raise RuntimeError("translator died")

        with self.assertRaises(RuntimeError):
            build_py.publish_generated(self.root, failing)
        self.assertEqual((self.gen / "table.c").read_text(), "old\n")
        self.assertEqual(sorted(p.name for p in self.gen.parent.iterdir()), ["gen", "symbols.json"])

    def test_successful_translation_publishes_sources_header_and_symbols(self):
        def succeeding(stage):
            (stage / "table.c").write_text("new\n")
            (stage / "chunk_000.c").write_text("void fn_00401000(void) {}\n")
            (stage / "symbols.json").write_text('{"generation": 2}\n')

        build_py.publish_generated(self.root, succeeding)
        self.assertEqual((self.gen / "table.c").read_text(), "new\n")
        self.assertTrue((self.gen / "chunk_000.c").is_file())
        self.assertEqual((self.gen / "x86.h").read_text(), "// x86\n")
        self.assertEqual((self.root / "build/recomp/symbols.json").read_text(), '{"generation": 2}\n')
        self.assertEqual(sorted(p.name for p in self.gen.parent.iterdir()), ["gen", "symbols.json"])

    def test_first_translation_works_without_a_previous_tree(self):
        shutil.rmtree(self.gen)
        build_py.publish_generated(self.root, lambda stage: (stage / "table.c").write_text("first\n"))
        self.assertEqual((self.gen / "table.c").read_text(), "first\n")


if __name__ == "__main__":
    unittest.main()
