"""Populous's game.toml renders the values the kit's hooks expect."""
import importlib.util
import json
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

    def test_controls_mapping(self):
        controls = self.cfg["controls"]
        # The pad alone: the game's interface is a full-height left strip the
        # kit's "pad+keys" keyboard half would bury.
        self.assertEqual(controls["default_layout"], "pad")
        self.assertEqual(controls["pad"], "mapped")
        mapped = controls["mapped"]
        # The pointer is on the left stick, opposite the face diamond; the
        # right stick and a real pad's dpad hold the Keycard's camera keys.
        self.assertEqual(mapped["left_stick"], "cursor")
        self.assertEqual(mapped["right_stick"], "arrows")
        self.assertEqual(mapped["dpad"], "arrows")
        self.assertEqual(
            {key: mapped[key] for key in ("cross", "circle", "square", "triangle")},
            {"cross": "mouse_left", "circle": "mouse_right",
             "square": "key:Space", "triangle": "key:H"})
        # The two click modifiers touch cannot hold, then zoom out/in.
        self.assertEqual(mapped["l1"], "key:LShift")
        self.assertEqual(mapped["r1"], "key:LCtrl")
        self.assertEqual(mapped["l2"], "key:Minus")
        self.assertEqual(mapped["r2"], "key:Equals")
        self.assertEqual(mapped["select"], "key:Return")
        self.assertEqual(mapped["start"], "key:Escape")
        self.assertIn('#define RECOMP_CONTROLS_DEFAULT_LAYOUT "pad"', self.header)
        self.assertIn("triangle=key:H", self.header)
        self.assertIn("left_stick=cursor", self.header)

    def test_shipped_tablet_pad_layout(self):
        """layouts/pad.tablet.json overrides only the tablet pad, and every
        control in it clears the game's left interface strip."""
        path = ROOT / "layouts" / "pad.tablet.json"
        layout = json.loads(path.read_text())
        self.assertEqual(layout["name"], "pad")
        self.assertEqual(sorted(p.name for p in (ROOT / "layouts").glob("*.json")),
                         ["pad.tablet.json"])
        groups = {group["id"]: group for group in layout["groups"]}
        self.assertEqual(sorted(groups), ["buttons", "hotkeys", "sticks", "tabs"])
        # The strip is about 16% of the image's width; 210pt is that share of
        # the narrowest tablet safe area the kit draws into, rounded up.
        strip = 210
        for group in layout["groups"]:
            for control in group["controls"]:
                anchor = control.get("anchor", group.get("anchor", "bottom-left"))
                if not anchor.endswith("left"):
                    continue
                left = control.get("x", group.get("x", 0))
                self.assertGreaterEqual(
                    left, strip,
                    "%s %s sits over the interface strip" % (anchor, control))
        # No on-screen dpad: it would only repeat the right stick, and there
        # is no room for it beside the strip.
        kinds = [control["kind"] for group in layout["groups"] for control in group["controls"]]
        self.assertNotIn("dpad", kinds)
        # The Keycard keys the eleven pad buttons cannot hold.
        scancodes = [control["scancode"] for control in groups["hotkeys"]["controls"]]
        self.assertEqual(scancodes, ["1", "2", "3", "4", "5", "6",
                                     "Z", "X", "C", "V", "P", "F1"])

    def test_bundle_exclusions_and_setup(self):
        self.assertIn("Fmv", self.cfg["bundle"]["exclude"])
        self.assertIn("*.dll", self.cfg["bundle"]["exclude"])
        self.assertEqual(self.cfg["setup"]["required_dirs"], ["data", "levels", "objects", "sound"])
        self.assertTrue(self.cfg["setup"]["annotations_url"].endswith("pop3-rev.git"))


if __name__ == "__main__":
    unittest.main()
