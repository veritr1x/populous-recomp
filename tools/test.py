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


def run(args, env=None):
    """Propagate a test command's failure instead of treating missing coverage as success."""
    subprocess.run([str(arg) for arg in args], cwd=ROOT, env=env, check=True)


def mods(env):
    """Generate the real entity fixture locally, then test mods under one build lock."""
    if env.get("BUILDLOCK_HELD") != "1":
        run(["sh", "tools/recomp/buildlock.sh", "run", ROOT, "mod tests",
             sys.executable, __file__, "--mods"], env)
        return
    run(["sh", "tools/recomp/parity_build.sh"], env)
    output = ROOT / "build/tests"
    output.mkdir(parents=True, exist_ok=True)
    case = Path(tempfile.mkdtemp(prefix="mod-fixture-", dir=output))
    fixture_env = dict(env, POPM_NO_MODS="1", POP_RECOMP_FIXTURE="frames:32",
                       POP_RECOMP_OUT=str(case / "snapshots"),
                       POPM_PROFILE_DIR=str(case / "profile"),
                       POPM_REGISTRY=str(case / "registry.json"),
                       POPM_RUN_RECORD=str(case / "run.json"))
    print(f"Mod fixture diagnostics: {case}", flush=True)
    with (case / "fixture.log").open("w") as log:
        result = subprocess.run([str(ROOT / "build/recomp/pop_fixture")], cwd=ROOT,
                                env=fixture_env, stdout=log, stderr=subprocess.STDOUT,
                                timeout=120)
    result.check_returncode()
    snapshot = case / "snapshots/frame32._data_00598000.bin"
    if not snapshot.is_file():
        raise RuntimeError(f"The entity fixture was not captured; inspect {case}")
    run(["sh", "src/recomp/mods/build_tests.sh"],
        dict(env, POPM_TEST_GAME_VIEW_SNAPSHOT=str(snapshot)))


def gameplay():
    """Replay native Options and movement in a unique profile; retain diagnostics under build/."""
    run([sys.executable, "tools/build.py", "--target", "smoke"])
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
    print(f"Gameplay diagnostics: {case}", flush=True)
    with (case / "smoke.log").open("w") as log:
        result = subprocess.run([str(ROOT / "build/recomp/pop_smoke")], cwd=ROOT, env=env,
                                stdout=log, stderr=subprocess.STDOUT, timeout=300)
    result.check_returncode()
    text = (case / "smoke.log").read_text()
    for mode in ("800x600", "3840x2160", "640x480"):
        if f"display mode {mode} 16bpp" not in text:
            raise RuntimeError(f"The gameplay run did not reach {mode}; inspect {case}")
    if "all expectations met" not in text:
        raise RuntimeError(f"Gameplay assertions did not complete; inspect {case}")


def main():
    """Choose portable, compile-only or game-backed suites and check their prerequisites."""
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--native", action="store_true")
    group.add_argument("--mods", action="store_true")
    group.add_argument("--gameplay", action="store_true")
    group.add_argument("--compile-only", action="store_true")
    args = parser.parse_args()
    native = args.native or args.mods or args.gameplay or args.compile_only
    if native and platform.system() != "Darwin":
        parser.error("Native suites require macOS")
    if native and not args.compile_only and not (ROOT / "original/gog/D3DPopTB.exe").is_file():
        parser.error("This suite needs your game installation; run tools/setup.py first")
    if (args.mods or args.gameplay) and not (ROOT / "build/recomp/librecomp_gen.a").is_file():
        parser.error("Build the game with tools/build.py before running this suite")
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(("POPM_", "POP_RECOMP_", "POP_SMOKE_", "POP_HOST_"))}
    env["PY"] = sys.executable
    try:
        if args.gameplay:
            gameplay()
        elif args.mods:
            mods(env)
        elif args.native or args.compile_only:
            for module in ("runtime", "dx", "host"):
                command = ["sh", f"src/recomp/{module}/build_tests.sh"]
                if args.compile_only:
                    command.append("--no-run")
                run(command, env)
        else:
            run([sys.executable, "-m", "pytest", "-q", "tests/test_setup.py",
                 "tools/recomp/tests/test_mode_probe.py", "tools/recomp/tests/test_texture_pack.py",
                 "tools/recomp/tests/test_terrain_detail.py"], env)
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired, RuntimeError) as error:
        parser.exit(1, f"Tests failed: {error}\n")


if __name__ == "__main__":
    main()
