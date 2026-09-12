#!/usr/bin/env python3
"""Run explicit contributor suites, with game-backed checks isolated from player saves."""

import argparse
import importlib.util
import json
import os
from pathlib import Path
import platform
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools/recomp"))
import buildlock  # noqa: E402

spec = importlib.util.spec_from_file_location("build_py", ROOT / "tools/build.py")
build_py = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build_py)

PORTABLE_TESTS = [
    "tests/test_setup.py", "tests/test_build_py.py",
    "tools/recomp/tests/test_mode_probe.py", "tools/recomp/tests/test_texture_pack.py",
    "tools/recomp/tests/test_terrain_detail.py", "tools/recomp/tests/test_buildlock.py",
    "tools/recomp/tests/test_translate_publish.py",
]


def run(args, env=None):
    """Propagate a test command's failure instead of treating missing coverage as success."""
    subprocess.run([str(arg) for arg in args], cwd=ROOT, env=env, check=True)


def ctest(preset, labels, env):
    """Run the CTest entries whose label matches `labels` (a regex)."""
    run([build_py.cmake_tool("ctest"), "--preset", preset, "-L", labels, "--output-on-failure"], env)


def native(preset, env, jobs, run_tests):
    """Build every test binary this platform has; run the suites that need no snapshot."""
    build_py.configure(preset)
    build_py.build(preset, ["check_binaries"], jobs)
    if run_tests:
        ctest(preset, "nogame|game|gpu", env)


def mods(preset, env, jobs):
    """Generate the real entity fixture locally, then run the mod suites under one build lock."""
    with buildlock.BuildLock(ROOT, "mod tests"):
        build_py.configure(preset)
        build_py.build(preset, ["pop_fixture", "mods_tests", "present_events_tests"], jobs)
        output = ROOT / "build/tests"
        output.mkdir(parents=True, exist_ok=True)
        case = Path(tempfile.mkdtemp(prefix="mod-fixture-", dir=output))
        fixture_env = dict(env, POPM_NO_MODS="1", POP_RECOMP_FIXTURE="frames:32",
                           POP_RECOMP_OUT=str(case / "snapshots"),
                           POPM_PROFILE_DIR=str(case / "profile"),
                           POPM_REGISTRY=str(case / "registry.json"),
                           POPM_RUN_RECORD=str(case / "run.json"))
        print("Mod fixture diagnostics: %s" % case, flush=True)
        with (case / "fixture.log").open("w") as log:
            result = subprocess.run([str(ROOT / "build/recomp/pop_fixture")], cwd=ROOT, env=fixture_env,
                                    stdout=log, stderr=subprocess.STDOUT, timeout=120)
        result.check_returncode()
        snapshot = case / "snapshots/frame32._data_00598000.bin"
        if not snapshot.is_file():
            raise RuntimeError("The entity fixture was not captured; inspect %s" % case)
        ctest(preset, "mods", dict(env, POPM_TEST_GAME_VIEW_SNAPSHOT=str(snapshot)))


def gameplay(jobs):
    """Replay native Options and movement in a unique profile; retain diagnostics under build/."""
    run([sys.executable, "tools/build.py", "--target", "smoke", "--jobs", jobs])
    spec = importlib.util.spec_from_file_location("mode_probe", ROOT / "tools/recomp/mode_probe.py")
    probe = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(probe)
    output = ROOT / "build/gameplay"
    output.mkdir(parents=True, exist_ok=True)
    case = Path(tempfile.mkdtemp(prefix="options-", dir=output))
    script = (ROOT / "tools/recomp/smoke/native-options.script").read_text()
    pack = ROOT / "build/texture-pack"
    manifest = json.loads((pack / "manifest.json").read_text()) if (pack / "manifest.json").is_file() else {}
    # Material detail ships from project artwork. Original-game replacement
    # textures are optional, so only assert HD replacements when some exist.
    absent = []
    if not manifest.get("textures"):
        absent.append("expect hd_draws")
    if not manifest.get("terrain_detail"):
        absent.append("expect terrain_detail_draws")
    script = "\n".join(line for line in script.splitlines()
                       if not line.startswith(tuple(absent))) + "\n"
    path = case / "input.script"
    path.write_text(script)
    env = probe.probe_environment((640, 480, 16), path, case)
    env.pop("POP_SMOKE_CLASSIC_PROBE", None)
    env.update(POP_SMOKE_DRAWABLE="1280x960",
               POPM_DDRAW_MODES="640x480x8,640x480x16,800x600x16,3840x2160x16",
               POPM_CORE_MODS_DIR=str(ROOT / "build/recomp/mods/core"),
               POPM_TEXTURE_PACK_DIR=str(pack))
    print("Gameplay diagnostics: %s" % case, flush=True)
    with (case / "smoke.log").open("w") as log:
        result = subprocess.run([str(ROOT / "build/recomp/pop_smoke")], cwd=ROOT, env=env,
                                stdout=log, stderr=subprocess.STDOUT, timeout=300)
    result.check_returncode()
    text = (case / "smoke.log").read_text()
    for mode in ("800x600", "3840x2160", "640x480"):
        if "display mode %s 16bpp" % mode not in text:
            raise RuntimeError("The gameplay run did not reach %s; inspect %s" % (mode, case))
    if "all expectations met" not in text:
        raise RuntimeError("Gameplay assertions did not complete; inspect %s" % case)


def main():
    """Choose portable, compile-only or game-backed suites and check their prerequisites."""
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--native", action="store_true", help="Build and run the native suites")
    group.add_argument("--mods", action="store_true", help="Real game-backed mod tests")
    group.add_argument("--gameplay", action="store_true", help="Scripted native Options and gameplay run")
    group.add_argument("--compile-only", action="store_true", help="Build the native test binaries only")
    parser.add_argument("--preset", default=build_py.default_preset())
    parser.add_argument("--jobs", type=int, default=min(os.cpu_count() or 2, 8))
    args = parser.parse_args()
    game_backed = args.mods or args.gameplay
    if game_backed and platform.system() != "Darwin":
        parser.error("Game-backed suites require macOS")
    if (game_backed or args.native) and not (ROOT / "original/gog/D3DPopTB.exe").is_file():
        parser.error("This suite needs your game installation; run tools/setup.py first, "
                     "or run `ctest --preset <preset> -L nogame` for the portable suites")
    if game_backed and not build_py.archive_path().is_file():
        parser.error("Build the game with tools/build.py before running this suite")
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(("POPM_", "POP_RECOMP_", "POP_SMOKE_", "POP_HOST_"))}
    env["PY"] = sys.executable
    try:
        if args.gameplay:
            gameplay(args.jobs)
        elif args.mods:
            mods(args.preset, env, args.jobs)
        elif args.native or args.compile_only:
            native(args.preset, env, args.jobs, run_tests=args.native)
        else:
            run([sys.executable, "-m", "pytest", "-q"] + PORTABLE_TESTS, env)
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired, RuntimeError, TimeoutError) as error:
        parser.exit(1, "Tests failed: %s\n" % error)


if __name__ == "__main__":
    main()
