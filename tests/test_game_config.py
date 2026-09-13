"""Populous's game.toml renders the values the kit's hooks expect."""
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
KIT = ROOT / "kit"


def load_module(name):
    spec = importlib.util.spec_from_file_location(name, KIT / "tools" / (name + ".py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


game_config = load_module("game_config")
gen_game_config = load_module("gen_game_config")


class PopulousConfigTests(unittest.TestCase):
    def setUp(self):
        self.cfg = game_config.load(ROOT)
        self.header = gen_game_config.render_header(self.cfg)

    def test_identity(self):
        self.assertEqual(self.cfg["game"]["id"], "populous")
        self.assertEqual(self.cfg["game"]["executable"], "D3DPopTB.exe")
        self.assertEqual(self.cfg["game"]["entry_point"], 0x0055D6C0)
        self.assertEqual(self.cfg["game"]["image_base"], 0x00400000)
        self.assertIn('#define RECOMP_GUEST_ROOT "C:\\\\Populous"', self.header)
        self.assertIn('#define RECOMP_APP_NAME "PopRecomp"', self.header)
        self.assertEqual(self.cfg["developer_exe_path"], (ROOT / "original/gog/D3DPopTB.exe").resolve())
        self.assertEqual(self.cfg["listings_path"], (ROOT / "analysis/decompiled/D3DPopTB.exe").resolve())

    def test_hooks_and_globals(self):
        self.assertIn("#define RECOMP_HOOK_FRAME_CLOCK_BEGIN 0x004a45a3u", self.header)
        self.assertIn("#define RECOMP_HOOK_CURSOR_SURFACE_PTRS_COUNT 2", self.header)
        self.assertIn("#define RECOMP_HOOK_CURSOR_SURFACE_PTRS {0x005d5718u, 0x005d571cu}", self.header)
        self.assertIn("#define RECOMP_HOOK_MOUSE_VTABLE 0x00591b6cu", self.header)
        self.assertIn("#define RECOMP_HOOK_CAMERA 0x0074a350u", self.header)
        self.assertIn("#define RECOMP_GLOBAL_SIMULATION_TURN_ADDR 0x0089d188u", self.header)
        self.assertIn("#define RECOMP_GLOBAL_ENTITY_BASE_STRIDE 179u", self.header)
        self.assertIn("#define RECOMP_GLOBAL_ENTITY_BASE_COUNT 2000u", self.header)

    def test_bundle_exclusions_and_setup(self):
        self.assertIn("Fmv", self.cfg["bundle"]["exclude"])
        self.assertIn("*.dll", self.cfg["bundle"]["exclude"])
        self.assertEqual(self.cfg["setup"]["required_dirs"], ["data", "levels", "objects", "sound"])
        self.assertTrue(self.cfg["setup"]["annotations_url"].endswith("pop3-rev.git"))


if __name__ == "__main__":
    unittest.main()
