#!/usr/bin/env python3
"""Build the native app through CMake, regenerating original-game code only when needed."""

import argparse
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools/recomp"))
import buildlock  # noqa: E402

# What each --target builds. `plugins` is every mod plugin the tree ships.
TARGETS = {
    "app": ["PopRecomp"],
    "smoke": ["pop_smoke"],
    "headless": ["pop_headless"],
    "fixture": ["pop_fixture"],
    "gen": ["recomp_gen"],
    "plugins": ["plugins"],
}
MACOS_ONLY = {"app", "smoke", "headless"}
NEEDS_GEN = {"app", "smoke", "headless", "fixture", "gen"}


def default_preset(system=None):
    """The CMake preset for this operating system."""
    return {"Darwin": "macos", "Linux": "linux", "Windows": "windows"}[system or platform.system()]


def preset_name(preset, config):
    """Debug builds live in their own binary directory, so they are their own preset."""
    return preset if config == "Release" else preset + "-debug"


def archive_path(root=ROOT, system=None):
    """Where CMake writes the translated archive on this platform."""
    name = "recomp_gen.lib" if (system or platform.system()) == "Windows" else "librecomp_gen.a"
    return Path(root) / "build/recomp" / name


def cmake_tool(name):
    """Prefer the venv's pinned cmake/ctest beside this interpreter, then PATH."""
    beside = Path(sys.executable).parent / name
    if beside.exists():
        return str(beside)
    return shutil.which(name) or name


def configure(preset, extra=()):
    subprocess.run([cmake_tool("cmake"), "--preset", preset, "-DPython3_EXECUTABLE=" + sys.executable]
                   + list(extra), cwd=ROOT, check=True)


def build(preset, targets, jobs):
    subprocess.run([cmake_tool("cmake"), "--build", "--preset", preset, "--parallel", str(jobs), "--target"]
                   + list(targets), cwd=ROOT, check=True)


def publish_generated(root, translate):
    """Stage a translation, then publish gen/ and symbols.json by rename.

    `translate(stage_dir)` writes the sources and raises on failure; the
    published tree is untouched in that case. Publishing is renames only, so
    a reader under the same lock never sees half a generation."""
    root = Path(root)
    recomp = root / "build/recomp"
    recomp.mkdir(parents=True, exist_ok=True)
    gen, old = recomp / "gen", recomp / "gen.old"
    stage = recomp / ("gen.new.%d" % os.getpid())
    shutil.rmtree(stage, ignore_errors=True)
    stage.mkdir()
    try:
        translate(stage)
        # x86.h sits beside the generated sources so #include "x86.h" resolves.
        shutil.copy(root / "tools/recomp/runtime/x86.h", stage / "x86.h")
    except BaseException:
        shutil.rmtree(stage, ignore_errors=True)
        raise
    shutil.rmtree(old, ignore_errors=True)
    if gen.exists():
        gen.rename(old)
    stage.rename(gen)
    symbols = gen / "symbols.json"
    if symbols.is_file():
        temporary = recomp / ("symbols.json.new.%d" % os.getpid())
        shutil.copy(symbols, temporary)
        temporary.replace(recomp / "symbols.json")
    shutil.rmtree(old, ignore_errors=True)


def publish_tracked(root):
    """Replace translation/ with build/recomp/gen wholesale; the caller commits it."""
    root = Path(root)
    gen, tracked = root / "build/recomp/gen", root / "translation"
    shutil.rmtree(tracked, ignore_errors=True)
    tracked.mkdir()
    for path in sorted(gen.iterdir()):
        if path.suffix in (".c", ".h") or path.name == "symbols.json":
            shutil.copy(path, tracked / path.name)
    print("published %d files to translation/" % sum(1 for _ in tracked.iterdir()))


def run_translator(stage):
    subprocess.run([sys.executable, str(ROOT / "tools/recomp/translate.py"), "--out", str(stage),
                    "--report", str(ROOT / "build/recomp/translate-report.json")], cwd=ROOT, check=True)


def texture_pack():
    """Compile the redistributable material-detail layer when its inputs are newer.
    Original-game replacement textures remain optional, locally prepared pack entries."""
    detail = ROOT / "build/texture-pack/terrain-detail.popt"
    artwork = ROOT / "assets/terrain/materials-v1.png"
    compiler = ROOT / "tools/recomp/terrain_detail.py"
    if (not detail.is_file() or not (detail.parent / "manifest.json").is_file()
            or detail.stat().st_mtime < max(artwork.stat().st_mtime, compiler.stat().st_mtime)):
        subprocess.run([sys.executable, str(compiler), "--source", str(artwork),
                        "--output", str(detail.parent)], cwd=ROOT, check=True)


def parse_args(argv, system=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--regenerate", action="store_true", help="Regenerate and compile translated C")
    parser.add_argument("--publish-tracked", action="store_true",
                        help="Regenerate, then copy the translation into translation/ for committing")
    parser.add_argument("--target", choices=sorted(TARGETS), default="app")
    parser.add_argument("--jobs", type=int, default=min(os.cpu_count() or 2, 8))
    parser.add_argument("--preset", default=default_preset(system), help="CMake configure preset")
    parser.add_argument("--config", choices=("Release", "Debug"), default="Release")
    args = parser.parse_args(argv)
    if args.target in MACOS_ONLY and (system or platform.system()) != "Darwin":
        parser.error("The %s host currently builds on macOS; use --target fixture, gen or plugins elsewhere"
                     % args.target)
    if args.jobs < 1:
        parser.error("--jobs must be at least 1")
    return args, parser


def main():
    """Check inputs, translate under the build lock when needed, then configure and build."""
    args, parser = parse_args(sys.argv[1:])
    if args.publish_tracked:
        args.regenerate = True
    # The tracked translation lets a contributor without the game build the
    # hosts; regenerating still needs the game and its listings.
    if args.regenerate and not (ROOT / "original/gog/D3DPopTB.exe").is_file():
        parser.error("Prepare your own game installation with tools/setup.py first")
    preset = preset_name(args.preset, args.config)
    try:
        with buildlock.BuildLock(ROOT, "tools/build.py"):
            if args.target in NEEDS_GEN and args.regenerate:
                if not (ROOT / "analysis/decompiled/D3DPopTB.exe/functions.tsv").is_file():
                    parser.error("Translation listings are missing; run tools/setup.py without --link-only")
                publish_generated(ROOT, run_translator)
                if args.publish_tracked:
                    publish_tracked(ROOT)
            if args.target == "app":
                texture_pack()
            configure(preset)
            build(preset, TARGETS[args.target], args.jobs)
    except subprocess.CalledProcessError as error:
        parser.exit(error.returncode or 1, "Build failed; see the compiler output above.\n")
    except TimeoutError as error:
        parser.exit(1, "%s\n" % error)


if __name__ == "__main__":
    main()
