#!/usr/bin/env python3
"""Run the roots probe: defaults, overrides, empty overrides, a relocated
app-shaped directory and a negative control. Portable replacement for
roots_tests.sh."""
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def run(probe, args, env_overrides, expect_ok=True):
    env = {k: v for k, v in os.environ.items() if k not in ("RECOMP_CORE_MODS_DIR", "RECOMP_MODS_DIR")}
    env.update(env_overrides)
    r = subprocess.run([str(probe), *args], cwd=ROOT, env=env)
    if (r.returncode == 0) != expect_ok:
        raise SystemExit(f"FAIL: {probe} {args} env={env_overrides} exit {r.returncode}")


def main():
    src = Path(sys.argv[1])
    with tempfile.TemporaryDirectory(prefix="pop-core-roots.") as tmp:
        probe = Path(tmp) / src.name
        shutil.copy2(src, probe)
        run(probe, ["build/recomp/mods/core", "mods"], {})
        run(probe, ["/custom/core", "/custom/user"],
            {"RECOMP_CORE_MODS_DIR": "/custom/core", "RECOMP_MODS_DIR": "/custom/user"})
        run(probe, ["build/recomp/mods/core", "mods"], {"RECOMP_CORE_MODS_DIR": "", "RECOMP_MODS_DIR": ""})
        macos = Path(tmp) / "Relocated.app/Contents/MacOS"
        macos.mkdir(parents=True)
        relocated = macos / src.name
        shutil.copy2(src, relocated)
        # The layout reports the bundle's Resources directory, normalised, with
        # forward slashes on every platform.
        run(relocated, [(macos.parent / "Resources/mods/core").as_posix(), "mods"], {})
        run(relocated, ["/override", "mods"], {"RECOMP_CORE_MODS_DIR": "/override"})
        run(probe, ["/incorrect/core", "mods"], {}, expect_ok=False)
    print("PASS: default, overrides, empty overrides, relocated app and negative control")


if __name__ == "__main__":
    main()
