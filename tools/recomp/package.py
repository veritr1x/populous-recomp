#!/usr/bin/env python3
"""Package a finished build into the release archive for one platform.

macOS: PopRecomp-<v>-macos-arm64.zip holding PopRecomp.app (as finish_bundle.py
built it), LICENSE, NOTICE, README.txt. Windows/Linux: a PopRecomp folder with
the executable, resources/{mods/core,texture-pack,classic-modes.json}, LICENSE,
NOTICE, README.txt, zipped (Windows) or tar.gz'd (Linux)."""
import argparse
import os
import shutil
import subprocess
import sys
import tarfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DOCS = ["LICENSE", "NOTICE"]
SUFFIX = {"macos": "macos-arm64", "linux": "linux-x64", "windows": "windows-x64"}


def stage_resources(dest, cc):
    resources = dest / "resources"
    resources.mkdir(parents=True)
    shutil.copy(ROOT / "tools/recomp/baseline/classic-modes.json", resources / "classic-modes.json")
    shutil.copy(symbols_json(), resources / "symbols.json")
    subprocess.run([sys.executable, str(ROOT / "tools/recomp/build_core.py"),
                    "--dest", str(resources / "mods/core"), "--cc", cc], check=True, cwd=ROOT)
    pack = ROOT / "build/texture-pack"
    if (pack / "manifest.json").is_file():
        subprocess.run([sys.executable, str(ROOT / "tools/recomp/package_texture_pack.py"),
                        str(pack), str(resources / "texture-pack")], check=True, cwd=ROOT)


def symbols_json():
    """The translation index the mod loader reads: a regenerated one, else the tracked one."""
    fresh = ROOT / "build/recomp/symbols.json"
    return fresh if fresh.is_file() else ROOT / "translation/symbols.json"


def add_docs(dest):
    for name in DOCS:
        shutil.copy(ROOT / name, dest / name)
    shutil.copy(ROOT / "tools/recomp/release/README.txt", dest / "README.txt")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--preset", choices=sorted(SUFFIX), required=True)
    ap.add_argument("--version", required=True)
    ap.add_argument("--out", type=Path, default=ROOT / "dist")
    ap.add_argument("--cc", default=os.environ.get("CC", "clang"))
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    stage = args.out / f"stage-{args.preset}"
    shutil.rmtree(stage, ignore_errors=True)
    stage.mkdir()
    name = f"PopRecomp-{args.version}-{SUFFIX[args.preset]}"
    if args.preset == "macos":
        app = ROOT / "build/PopRecomp.app"
        subprocess.run(["codesign", "--verify", "--deep", "--strict", str(app)], check=True)
        shutil.copytree(app, stage / "PopRecomp.app", symlinks=True)
        add_docs(stage)
        archive = args.out / f"{name}.zip"
        archive.unlink(missing_ok=True)
        # ditto keeps the bundle's metadata and signature; zipfile does not.
        # The archive opens to PopRecomp.app, LICENSE, NOTICE and README.txt.
        subprocess.run(["ditto", "-c", "-k", "--sequesterRsrc", str(stage), str(archive)], check=True)
    else:
        folder = stage / "PopRecomp"
        folder.mkdir()
        exe = ROOT / ("build/recomp/PopRecomp.exe" if args.preset == "windows"
                      else "build/recomp/PopRecomp")
        shutil.copy(exe, folder / exe.name)
        stage_resources(folder, args.cc)
        add_docs(folder)
        if args.preset == "windows":
            archive = args.out / f"{name}.zip"
            with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as z:
                for path in sorted(folder.rglob("*")):
                    z.write(path, path.relative_to(stage))
        else:
            archive = args.out / f"{name}.tar.gz"
            with tarfile.open(archive, "w:gz") as t:
                t.add(folder, arcname="PopRecomp")
    shutil.rmtree(stage)
    print(archive)


if __name__ == "__main__":
    main()
