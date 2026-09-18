# Display and performance

Enhanced uses the resolution selected in the game's graphics settings and
renders the world into a drawable-sized scene target. Switching back from
Classic preserves that selected mode. The compositor combines the world, integer-scaled UI elements,
and the UI-space overlay pass. Classic retains the selected guest mode and
aspect-fits its final image, with nearest-neighbour scaling and letterboxing.
Neither setting changes simulation. Allocation failure halves the Enhanced
scene target down to guest size; it does not select Classic automatically.

The game's Options screen includes Enhanced, Display and Mods tabs alongside
Game, Sound and Graphics. F10 and macOS Populous > Settings… (Command-comma)
open these same native controls, from the front end or the paused game. Use
mouse clicks or the original keyboard navigation; left/right changes values.
F11 cycles the performance overlay. All built-in changes save automatically
and apply without restarting. Mod settings use the same live settings API.
The native Options font, glyph destinations and hit rectangles scale with the
selected game resolution, including 4K. The active tab stays highlighted.

The settings pages provide these rows:

| Row | Choices | When applied |
| --- | --- | --- |
| Rendering | Enhanced (default), Classic | Next frame boundary; no level reload |
| Wide view | on (default), off | Next frame boundary; retains resolution and sky corrections. Off shows the 4:3 scene boxed on a wider window |
| Display | windowed, borderless, fullscreen | Host window request |
| Screen Resolution (Graphics tab) | 640×480, 800×600, 1024×768, 1280×720, 1920×1080, 2560×1440, 3840×2160 (16-bit) | Immediately rebuilds the active game's surfaces; the front end stores the next game's resolution |
| Frame limit | original, 40, 60, 120 FPS | Immediately |
| Performance overlay | off, counters, graph | Immediately |
| Controls | pad (default), keys, pad+keys, hidden | Immediately; the on-screen controls appear on touch devices |
| Controls size | small, medium, large | Immediately |
| Controls opacity | 20-100% | Immediately |
| Button haptics | on, off | Immediately |
| Edit controls | opens the on-device layout editor | Immediately |
| Textures | HD pack, original | Next draws, including terrain detail. A pack built without local HD inputs holds only the terrain detail, and the row then reads Terrain detail: on, off |
| World filtering | original, trilinear, 4x, 8x, 16x anisotropic | Next draws |

The game's **Options → Graphics → Screen Resolution** row is the single
resolution control and offers these seven resolutions in both rendering modes.
The Display tab contains window mode, frame limit, the performance overlay and
the on-screen controls' rows. game.toml `[settings] rows` names the rows shown;
UI scale is left out because Populous scales its sidebar with the world, so
the interface is always sized automatically.
The frontend stays at 640×480;
the selection takes effect when the game creates the level's surfaces and is
saved for the next launch. Quick Defaults retains the original game's presets,
so choose the resolution after applying a preset.

The native runtime enables the original full resolution list and lifts the
DirectDraw callback's 1600-pixel limit for the supported RGB565 modes. It retains
the callback's pixel-format checks and caps enumeration at the smaller guest
table's 48-entry capacity. The Graphics tab uses this list and saves the
CONFIG00 selection. Switching rendering mode preserves it.

Above the original supported camera modes, rendered camera zoom follows the
selected height relative to the active VCONFIG camera record's reference height.
This also covers the opening flyby, whose record still describes 640×480.
Camera updates do not compound the scale or rewrite saved zoom preferences.
Terrain projection and picking use the same camera value. Enhanced preserves a
selected widescreen projection when the host window has a narrower aspect ratio.

The committed [probe results](../tools/recomp/baseline/classic-modes.json) cover
the seven shipping modes on the recorded machine, with native compatibility
hooks enabled and isolated settings. `sh tools/recomp/mode_probe.sh` probes the full
24-candidate matrix; `tools/recomp/mode_probe.sh --mode 3840x2160x16` probes a
single mode without replacing the baseline. A missing list is
labelled as the baseline fallback; a valid list with no passing modes disables
Classic. At 640×480, auto UI scale is 2 at 1080p and 4 at 2160p; at 800×600,
it is 1 and 3 respectively. Menu pages stay
centred as a unit. The guest cursor is never anchored.

Sky and cloud coverage use the active screen width, height and HUD origin,
matching the widened terrain projection at the selected resolution. UI extraction
also carries those surface dimensions. A guest resolution, scene-domain or
rendering-mode change invalidates retained scene textures, so a frame without
new world draws cannot reuse the previous mode's image. The remaining 480 in
the cloud mesh transform describes the original asset's reference coordinates.
At higher guest resolutions, sky and cloud vertices also scale from the active
camera record's reference height. Their lower edges extend to the bottom of the
selected surface, including native widescreen modes and Classic, so coverage
does not depend on the host being wider than the guest.

At a gameplay screen edge, pointer correction reaches the exact camera-scroll
coordinate instead of stopping within the ordinary four-pixel pointing
deadband. The game requires row zero to pan upward; correction remains damped
and the pointer can move back into the scene immediately. Idle pointer wakeups
do not advance the correction loop's expected position; only delivered motion
does, so a stationary physical pointer can finish moving the game cursor to its
target without its pending correction being cancelled.
