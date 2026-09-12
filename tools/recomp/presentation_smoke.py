#!/usr/bin/env python3
"""Offscreen gameplay/input checks. These are not live FPS measurements."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
MODES = {
    'enhanced-1080': ('1920x1080', 0, 1, 'visible'),
    'enhanced-4k': ('3840x2160', 0, 1, 'visible'),
    'enhanced-ultrawide': ('2520x1080', 0, 1, 'visible'),
    'enhanced-4by3': ('1440x1080', 0, 1, 'hidden'),
    'classic-1080': ('1920x1080', 1, 1, 'hidden'),
    'enhanced-wide-off': ('1920x1080', 0, 0, 'hidden'),
}

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--modes', nargs='+', choices=MODES, default=list(MODES))
    parser.add_argument('--core', type=Path, default=ROOT/'build/recomp/mods/core')
    parser.add_argument('--out', type=Path)
    args = parser.parse_args()
    binary = ROOT/'build/recomp/pop_smoke'
    if not binary.is_file() or not args.core.is_dir():
        parser.error('Build the smoke host and core mods first; see docs/testing.md')
    parent = ROOT/'build/presentation-checks'
    parent.mkdir(parents=True, exist_ok=True)
    out = args.out.resolve() if args.out else Path(tempfile.mkdtemp(prefix='run-', dir=parent))
    if args.out:
        out.mkdir(parents=True, exist_ok=False)
    script = (ROOT/'tools/recomp/smoke/gate-c.script').read_text()
    # Keep the measured selection/walk and simulation assertions. Capture the
    # actual composed texture as well; ordinary dump includes guest readbacks.
    script = script.replace('dump selected\n', 'dump selected\ndumpc selected_native\n')
    script = script.replace('expect textures>0', 'dumpc final_native\nexpect textures>0')
    (out/'input.script').write_text(script)
    results = []
    for name in args.modes:
        size, classic, wide, landmark = MODES[name]
        run = out/name
        for directory in ('frames', 'profile', 'user-mods'):
            (run/directory).mkdir(parents=True)
        (run/'profile/mod-settings.json').write_text(json.dumps({
            'host.display/rendering': classic, 'host.display/wide_view': wide,
            'host.display/frame_limit': 0, 'host.display/performance_overlay': 0,
        }))
        env = os.environ.copy()
        env.pop('POPM_NO_MODS', None)
        env.update(LANDMARK=landmark, POP_RECOMP_PIN_CLOCK='1', POP_SMOKE_WINDOW_INPUT='1',
                   POP_SMOKE_DRAWABLE=size, POP_SMOKE_SIM_REGIONS='1',
                   POPM_CORE_MODS_DIR=str(args.core.resolve()), POPM_MODS_DIR=str(run/'user-mods'),
                   POPM_PROFILE_DIR=str(run/'profile'), POPM_RUN_RECORD=str(run/'run.json'),
                   POP_HOST_DUMP_DIR=str(run/'frames'), POP_RECOMP_SCRIPT=str(out/'input.script'))
        print('RUN', name, flush=True)
        with (run/'smoke.log').open('w') as log:
            try:
                code = subprocess.run([str(binary)], cwd=ROOT, env=env, stdout=log,
                                      stderr=subprocess.STDOUT, timeout=600).returncode
            except subprocess.TimeoutExpired:
                code = 124
        result = dict(mode=name, drawable=size, exit=code, live_fps_verified=False)
        results.append(result)
        (out/'results.json').write_text(json.dumps(results, indent=2)+'\n')
        print('RESULT', name, code, flush=True)
    print('Evidence:', out)
    return int(any(r['exit'] for r in results))

if __name__ == '__main__':
    raise SystemExit(main())
