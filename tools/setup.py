#!/usr/bin/env python3
"""Link your Populous installation and export its listings with the kit in kit/.

    tools/setup.py --install /path/to/your/Populous --ghidra-home /path/to/ghidra

Every option is the kit's: see kit/tools/setup.py --help."""
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
KIT = ROOT / "kit"
if not (KIT / "tools/setup.py").is_file():
    sys.exit("The kit submodule is missing: run `git submodule update --init`")
sys.exit(subprocess.call([sys.executable, str(KIT / "tools/setup.py"), "--game-dir", str(ROOT)] + sys.argv[1:],
                         cwd=ROOT))
