"""tools/build.py argument handling and CMake invocation, without CMake or game files."""

import importlib.util
from pathlib import Path
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("build_py", Path(__file__).parents[1] / "tools/build.py")
build_py = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build_py)


class BuildPyTests(unittest.TestCase):
    def test_default_preset_follows_the_operating_system(self):
        self.assertEqual(build_py.default_preset("Darwin"), "macos")
        self.assertEqual(build_py.default_preset("Linux"), "linux")
        self.assertEqual(build_py.default_preset("Windows"), "windows")

    def test_debug_config_selects_the_debug_preset(self):
        self.assertEqual(build_py.preset_name("macos", "Release"), "macos")
        self.assertEqual(build_py.preset_name("linux", "Debug"), "linux-debug")

    def test_archive_path_per_platform(self):
        root = Path("/r")
        self.assertEqual(build_py.archive_path(root, "Darwin"), root / "build/recomp/librecomp_gen.a")
        self.assertEqual(build_py.archive_path(root, "Windows"), root / "build/recomp/recomp_gen.lib")

    def test_macos_hosts_are_refused_elsewhere(self):
        with self.assertRaises(SystemExit):
            build_py.parse_args(["--target", "smoke"], system="Linux")
        args, _ = build_py.parse_args(["--target", "fixture"], system="Linux")
        self.assertEqual(args.preset, "linux")

    def test_jobs_must_be_positive(self):
        with self.assertRaises(SystemExit):
            build_py.parse_args(["--jobs", "0"], system="Darwin")

    def test_build_passes_targets_and_parallelism_to_cmake(self):
        with patch.object(build_py.subprocess, "run") as run:
            build_py.build("macos", ["pop_smoke"], 6)
        command = run.call_args[0][0]
        self.assertIn("--build", command)
        self.assertEqual(command[command.index("--preset") + 1], "macos")
        self.assertEqual(command[command.index("--parallel") + 1], "6")
        self.assertEqual(command[command.index("--target") + 1:], ["pop_smoke"])


if __name__ == "__main__":
    unittest.main()
