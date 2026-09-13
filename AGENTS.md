# Working on Populous Recomp

Read README.md and CONTRIBUTING.md before a broad change. This repository
holds only what is Populous's: config, curated symbols, game headers, mods,
artwork, smoke scripts and docs. The runtime, translator, hosts and tools are
the kit in `kit/` (a git submodule of recomp-kit); edit those in the kit's own
repository and bump the submodule here. Game files and translations are
private local inputs under ignored `original/`, `analysis/` and `build/`.

- Keep changes focused; preserve unrelated local work and player profiles.
- Never replace 32-bit guest addresses with host pointers. Addresses belong
  in `game.toml` `[hooks]` and `globals.toml`, never in kit code.
- Edit translation rules in the kit, not `build/recomp/gen/`. Regenerate with
  `tools/build.py --regenerate` after changing the translator.
- Native code builds only through `tools/build.py` and `tools/test.py`, never
  by invoking compilers directly. Format kit sources with
  `.venv/bin/python kit/tools/format.py --write`.
- Run relevant suites from docs/testing.md and report exactly which checks
  ran; compilation and offscreen counters do not establish playable performance.
- Do not commit game assets, generated code, binaries, credentials, personal
  saves or run logs.
