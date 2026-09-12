#!/usr/bin/env python3
"""Finish an assembled app bundle: identity, resources, core mods, texture pack, signature."""

import argparse
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]


def rename_identity(plist_path, name, version):
    """A bundle built under another name gets its own identity; every bundle its version."""
    data = plistlib.loads(plist_path.read_bytes())
    if name != "PopRecomp":
        data.update(CFBundleName=name, CFBundleDisplayName=name, CFBundleExecutable=name,
                    CFBundleIdentifier="io.github.veritr1x.populousrecomp." + name.lower())
    if version:
        data.update(CFBundleShortVersionString=version.lstrip("v"), CFBundleVersion=version.lstrip("v"))
    plist_path.write_bytes(plistlib.dumps(data))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bundle", type=Path, required=True)
    parser.add_argument("--name", required=True)
    parser.add_argument("--cc", required=True)
    parser.add_argument("--version", default="")
    parser.add_argument("--pack", type=Path,
                        default=Path(os.environ.get("POPM_TEXTURE_PACK_DIR") or ROOT / "build/texture-pack"))
    args = parser.parse_args()
    contents = args.bundle / "Contents"
    resources = contents / "Resources"
    resources.mkdir(parents=True, exist_ok=True)
    rename_identity(contents / "Info.plist", args.name, args.version)
    # The committed probe list. A missing list is labelled a baseline fallback
    # by the settings layer; packaging never manufactures measurements.
    probes = ROOT / "tools/recomp/baseline/classic-modes.json"
    if probes.is_file():
        shutil.copy(probes, resources / "classic-modes.json")
    # The translation index the mod loader reads: a regenerated one, else the tracked one.
    fresh = ROOT / "build/recomp/symbols.json"
    shutil.copy(fresh if fresh.is_file() else ROOT / "translation/symbols.json", resources / "symbols.json")
    subprocess.run([sys.executable, str(ROOT / "tools/recomp/build_core.py"),
                    "--dest", str(resources / "mods/core"), "--cc", args.cc], check=True, cwd=ROOT)
    if (args.pack / "manifest.json").is_file():
        subprocess.run([sys.executable, str(ROOT / "tools/recomp/package_texture_pack.py"),
                        str(args.pack), str(resources / "texture-pack")], check=True, cwd=ROOT)
    # Seal after every resource is in place: the linker signature alone does
    # not cover the resource envelope.
    subprocess.run(["codesign", "--force", "--deep", "--sign", "-", str(args.bundle)], check=True)
    subprocess.run(["codesign", "--verify", "--deep", "--strict", str(args.bundle)], check=True)
    shown = args.bundle.relative_to(ROOT) if args.bundle.is_relative_to(ROOT) else args.bundle
    print("built %s" % shown)


if __name__ == "__main__":
    main()
