Populous: The Beginning - native port (PopRecomp)

You need your own copy of the game: the GOG release of Populous: The Beginning.
This download contains no game files.

First run
  macOS:   right-click PopRecomp.app, choose Open, confirm once (the app is not notarized).
  Windows: run PopRecomp.exe; if SmartScreen appears choose "More info" then "Run anyway".
  Linux:   run ./PopRecomp from the PopRecomp folder; a Vulkan driver must be installed.
A file dialog asks for D3DPopTB.exe. Pick it from your GOG installation. The game
starts, and the choice is remembered, so later launches need no dialog.

Only the GOG build is supported (SHA-256
815ba8a550f571c38b602cf3386f65aab942667a4a2d9c7096b3660deac2eacd).
Any other file is refused with a message that shows both digests.

Options
  PopRecomp --exe <path to D3DPopTB.exe>   skip the dialog and the saved path
  PopRecomp --version                      print the version and GPU backend
  PopRecomp --probe-layout                 print where resources and the profile live
  RECOMP_GPU_BACKEND=vulkan|metal             pick the GPU backend (macOS defaults to Metal)

Your settings and saves live in
  macOS:   ~/Library/Application Support/PopRecomp
  Windows: %APPDATA%\PopRecomp
  Linux:   ~/.local/share/PopRecomp (or $XDG_DATA_HOME/PopRecomp)
Replacing this folder with a newer download keeps them.

In game: F10 opens Options. Escape (held) releases the mouse. Command-Q or Alt-F4 quits.
Source, issues and releases: https://github.com/veritr1x/populous-recomp
