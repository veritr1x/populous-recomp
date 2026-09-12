#!/usr/bin/env python3
"""Build the native app, regenerating original-game code only when needed."""

import argparse
import os
from pathlib import Path
import platform
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def main():
    """Check inputs, serialize translation through the existing build lock, then link the host."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--regenerate", action="store_true", help="Regenerate and compile translated C")
    parser.add_argument("--target", choices=("app", "smoke", "headless"), default="app")
    parser.add_argument("--jobs", type=int, default=min(os.cpu_count() or 2, 8))
    args = parser.parse_args()
    if platform.system() != "Darwin":
        parser.error("The native host currently builds on macOS; portable tooling tests work elsewhere")
    if args.jobs < 1:
        parser.error("--jobs must be at least 1")
    if not (ROOT / "original/gog/D3DPopTB.exe").is_file():
        parser.error("Prepare your own game installation with tools/setup.py first")
    env = dict(os.environ, PY=sys.executable, JOBS=str(args.jobs))
    try:
        if args.regenerate or not (ROOT / "build/recomp/librecomp_gen.a").is_file():
            if not (ROOT / "analysis/decompiled/D3DPopTB.exe/functions.tsv").is_file():
                parser.error("Translation listings are missing; run tools/setup.py without --link-only")
            subprocess.run(["bash", "tools/recomp/build.sh"], cwd=ROOT, env=env, check=True)
        # This project-created detail layer is redistributable. Original-game
        # replacement textures remain optional, locally prepared pack entries.
        detail = ROOT / "build/texture-pack/terrain-detail.popt"
        artwork = ROOT / "assets/terrain/materials-v1.png"
        compiler = ROOT / "tools/recomp/terrain_detail.py"
        if (not detail.is_file() or not (detail.parent / "manifest.json").is_file()
                or detail.stat().st_mtime < max(artwork.stat().st_mtime, compiler.stat().st_mtime)):
            subprocess.run([sys.executable, str(compiler), "--source", str(artwork),
                            "--output", str(detail.parent)], cwd=ROOT, env=env, check=True)
        script = {"app": "app_build.sh", "smoke": "smoke_build.sh", "headless": "headless_build.sh"}[args.target]
        subprocess.run(["sh", f"tools/recomp/{script}"], cwd=ROOT, env=env, check=True)
    except subprocess.CalledProcessError as error:
        parser.exit(error.returncode, "Build failed; see the compiler output above.\n")


if __name__ == "__main__":
    main()
