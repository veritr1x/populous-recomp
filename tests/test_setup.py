"""Exercise first-time setup failures without game files, downloads or a Java process."""

import importlib.util
import hashlib
import subprocess
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("project_setup", Path(__file__).parents[1] / "tools/setup.py")
setup = importlib.util.module_from_spec(spec)
spec.loader.exec_module(setup)


class SetupTests(unittest.TestCase):
    def test_missing_executable_has_actionable_error(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(ValueError, "D3DPopTB.exe was not found"):
                setup.validate_game(Path(directory))

    def test_wrong_executable_is_rejected_before_linking(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "D3DPopTB.exe").write_bytes(b"synthetic invalid input")
            with self.assertRaisesRegex(ValueError, "Unsupported D3DPopTB.exe"):
                setup.validate_game(root)
            self.assertEqual(sorted(p.name for p in root.iterdir()), ["D3DPopTB.exe"])

    def test_existing_installation_is_never_replaced(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            first, second = root / "first", root / "second"
            first.mkdir(); second.mkdir()
            with patch.object(setup, "ROOT", root / "project"):
                setup.link_game(first)
                setup.link_game(first)  # Re-running setup for the same game is safe.
                with self.assertRaisesRegex(ValueError, "already points elsewhere"):
                    setup.link_game(second)
                self.assertEqual((setup.ROOT / "original/gog").resolve(), first.resolve())

    def test_dangling_installation_link_is_preserved(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "original").mkdir()
            link = root / "original/gog"
            link.symlink_to(root / "missing", target_is_directory=True)
            with patch.object(setup, "ROOT", root):
                with self.assertRaisesRegex(ValueError, "already points elsewhere"):
                    setup.link_game(root / "replacement")
                self.assertTrue(link.is_symlink())
                self.assertEqual(link.readlink(), root / "missing")

    def test_game_data_names_must_be_directories(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            data = b"synthetic supported executable"
            (root / "D3DPopTB.exe").write_bytes(data)
            for name in ("data", "levels", "objects", "sound"):
                (root / name).write_text("a file is not an installation directory")
            with patch.object(setup, "EXE_SHA256", hashlib.sha256(data).hexdigest()):
                with self.assertRaisesRegex(ValueError, "missing data/"):
                    setup.validate_game(root)

    def test_dirty_annotation_checkout_is_preserved(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            metadata = root / "analysis/pop3-rev"
            metadata.mkdir(parents=True)
            subprocess.run(["git", "init", str(metadata)], check=True, capture_output=True)
            work = metadata / "local-work.txt"
            work.write_text("keep these annotation edits")
            with patch.object(setup, "ROOT", root), patch.object(setup, "run") as run:
                with self.assertRaisesRegex(ValueError, "local changes"):
                    setup.prepare_annotations()
                run.assert_not_called()
                self.assertEqual(work.read_text(), "keep these annotation edits")

    def test_wrong_ghidra_version_does_not_launch_java(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "Ghidra").mkdir()
            (root / "Ghidra/application.properties").write_text("application.version=0.0\n")
            with patch.object(setup, "run") as run:
                with self.assertRaisesRegex(ValueError, "Use Ghidra"):
                    setup.export_listings(root, None, root / "metadata.xml")
                run.assert_not_called()


if __name__ == "__main__":
    unittest.main()
