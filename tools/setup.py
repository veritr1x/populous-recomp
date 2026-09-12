#!/usr/bin/env python3
"""Prepare private translation inputs from a contributor's own game installation.

Only a symlink to the installation and ignored analysis outputs are created.
The executable is hash checked before importing the pinned annotation metadata.
No game files are downloaded, changed, or included in the source repository.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
EXE_SHA256 = "815ba8a550f571c38b602cf3386f65aab942667a4a2d9c7096b3660deac2eacd"
ANNOTATIONS_URL = "https://github.com/hrttf111/pop3-rev.git"
ANNOTATIONS_REVISION = "60408e4e99b76ab2e5461897c8e1b33756360eaa"
GHIDRA_VERSION = "12.1.3"


def run(args, **kwargs):
    """Run one preparation step with argument boundaries preserved; fail on errors."""
    subprocess.run([str(arg) for arg in args], cwd=ROOT, check=True, **kwargs)


def validate_game(directory):
    """Require the supported executable and game-data directories before creating a link."""
    directory = directory.expanduser().resolve()
    executable = directory / "D3DPopTB.exe"
    if not executable.is_file():
        raise ValueError(f"D3DPopTB.exe was not found in {directory}")
    digest = hashlib.sha256(executable.read_bytes()).hexdigest()
    if digest != EXE_SHA256:
        raise ValueError(f"Unsupported D3DPopTB.exe: SHA-256 {digest}; expected {EXE_SHA256}")
    names = {entry.name.lower() for entry in directory.iterdir() if entry.is_dir()}
    for required in ("data", "levels", "objects", "sound"):
        if required not in names:
            raise ValueError(f"Game installation is missing {required}/")
    return directory


def link_game(directory):
    """Reuse the same installation link; refuse to replace another installation or directory."""
    directory = directory.expanduser().resolve()
    destination = ROOT / "original/gog"
    if destination.exists() or destination.is_symlink():
        if destination.resolve() != directory:
            raise ValueError(f"{destination} already points elsewhere; move it aside explicitly")
        return
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.symlink_to(directory, target_is_directory=True)


def prepare_annotations():
    """Fetch a fixed metadata revision into ignored storage without modifying a dirty checkout."""
    directory = ROOT / "analysis/pop3-rev"
    created = not directory.exists()
    if created:
        run(["git", "clone", "--no-checkout", ANNOTATIONS_URL, directory])
    elif not (directory / ".git").exists():
        raise ValueError(f"{directory} exists but is not a Git checkout")
    changes = subprocess.check_output(
        ["git", "-C", str(directory), "status", "--porcelain"], text=True
    )
    # A new --no-checkout clone reports deleted files until its first checkout.
    revision = subprocess.run(
        ["git", "-C", str(directory), "rev-parse", "--verify", "HEAD"],
        text=True, capture_output=True, check=False,
    )
    if changes and not created:
        raise ValueError("Annotation checkout has local changes; preserve them before preparing inputs")
    if revision.returncode or revision.stdout.strip() != ANNOTATIONS_REVISION:
        run(["git", "-C", directory, "fetch", "origin", ANNOTATIONS_REVISION])
    run(["git", "-C", directory, "checkout", "--detach", ANNOTATIONS_REVISION])
    return directory / "backup/backup.xml"


def export_listings(ghidra, java_home, annotations):
    """Import the verified game and export listings in a disposable Ghidra project."""
    ghidra = ghidra.expanduser().resolve()
    properties = ghidra / "Ghidra/application.properties"
    if not properties.is_file() or f"application.version={GHIDRA_VERSION}\n" not in properties.read_text():
        raise ValueError(f"Use Ghidra {GHIDRA_VERSION}; set --ghidra-home to its extracted directory")
    env = dict(os.environ)
    if java_home:
        env["JAVA_HOME"] = str(java_home.expanduser().resolve())
        env["PATH"] = str(Path(env["JAVA_HOME"]) / "bin") + os.pathsep + env.get("PATH", "")
    project = ROOT / "analysis/ghidra"
    output = ROOT / "analysis/decompiled"
    project.mkdir(parents=True, exist_ok=True)
    output.mkdir(parents=True, exist_ok=True)
    run([
        ghidra / "support/analyzeHeadless", project, "PopulousRecomp",
        "-import", ROOT / "original/gog/D3DPopTB.exe", "-noanalysis", "-deleteProject",
        "-scriptPath", ROOT / "tools", "-postScript", "ImportAnnotations.java", annotations,
        "-postScript", "ExportProgram.java", output,
    ], env=env)
    index = output / "D3DPopTB.exe/functions.tsv"
    if not index.is_file() or len(index.read_text().splitlines()) < 2:
        raise ValueError("Ghidra did not export a function index; inspect its error output")
    (output / "inputs.json").write_text(json.dumps({
        "executable_sha256": EXE_SHA256,
        "annotations_revision": ANNOTATIONS_REVISION,
        "ghidra_version": GHIDRA_VERSION,
    }, indent=2) + "\n")


def main():
    """Validate local prerequisites, preserve existing inputs, and prepare reproducible listings."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game-dir", type=Path, required=True, help="Your installed game directory")
    parser.add_argument("--ghidra-home", type=Path, default=os.environ.get("GHIDRA_HOME"))
    parser.add_argument("--java-home", type=Path, default=os.environ.get("JAVA_HOME"))
    parser.add_argument("--link-only", action="store_true", help="Validate/link game data without exporting")
    args = parser.parse_args()
    try:
        directory = validate_game(args.game_dir)
        if not args.link_only and not args.ghidra_home:
            raise ValueError("Set --ghidra-home or GHIDRA_HOME; see CONTRIBUTING.md")
        link_game(directory)
        if not args.link_only:
            export_listings(args.ghidra_home, args.java_home, prepare_annotations())
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"Setup failed: {error}\n")
    print("Game inputs ready. Next: .venv/bin/python tools/build.py")


if __name__ == "__main__":
    main()
