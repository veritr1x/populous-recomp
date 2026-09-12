# Working on Populous Recomp

Read README.md, CONTRIBUTING.md and docs/code-guide.md before a broad change.
This repository contains the native runtime and translator. Game files and
translations are private local inputs under ignored original/, analysis/ and build/.

- Keep changes focused; preserve unrelated local work and player profiles.
- Comment major functions and unusual guest layout, timing or ownership rules.
- Never replace 32-bit guest addresses with host pointers. Respect the cooperative
  scheduler baton and the immutable frame boundary described in docs/architecture.md.
- Edit translation rules, not build/recomp/gen/. Regenerate after changing the translator.
- Format first-party native source with `.venv/bin/python tools/format.py --write`.
  Preserve vendored code and its notices.
- Run relevant suites from docs/testing.md. Report exactly which checks ran;
  compilation and offscreen counters do not establish playable performance.
- Do not commit game assets, generated code, binaries, credentials, personal saves
  or run logs. Run `.venv/bin/python tools/check_repo.py` on staged source changes.
- Keep setup/build instructions reproducible from a clean checkout. Update the
  changelog for user-visible behavior and preserve the single Graphics resolution control.
