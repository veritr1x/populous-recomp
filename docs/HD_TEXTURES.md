# Full-color textures and HD packs

Enhanced rendering accepts RGBA8 textures up to 4096×4096 and keeps their
8-bit color channels through the Metal world target and compositor. The
old upload routine read every non-indexed pixel as a 16-bit word, even when
a mod supplied RGBA8. That truncated the blue and alpha channels and read
only half of each row. Both 32-bit texture formats are now advertised by
the Direct3D shim, after the original four 16-bit formats.

The game's software drawing and CONFIG00 mode list still use the original
8/16-bit compatibility buffers. Selecting a fake 32-bit guest mode would
break translated routines that increment pointers by two bytes. Enhanced
world textures, lighting, blending and displayed output use RGBA8/BGRA8;
required guest readbacks convert into the guest's format. Classic and
legacy replay retain original textures. Host surface uploads, CPU payloads,
UI extraction and readbacks also handle four-byte 24/32-bit storage, for
native callers and future software conversions. This does not convert every
original software rasterizer into a 32-bit rasterizer.

## Controls

Options > Enhanced (also accessible with F10) exposes two live, persisted options:

- **Textures: HD pack / original** selects replacements for Enhanced world
  draws. Switching does not mutate guest surfaces or lose revision leases.
  It also enables/disables the shared terrain material-detail layer.
- **World filtering: original / trilinear / 4x / 8x / 16x anisotropic** applies
  to complete opaque terrain tiles. Atlas subregions, UI, sprites and Classic
  retain the guest sampler. The default is 8x.

The renderer uses normalized UVs and the actual render-target dimensions.
The selected resolution determines the sampling footprint and mip level;
there is no 640×480 texture-scaling assumption. Small sprites and terrain
textures are enlarged proportionally, not each allocated as a 4K image.

## Building a local pack

Original and derivative game assets remain local, outside Git. Install the
asset tool dependencies in a dedicated virtual environment:

```sh
python3 -m venv build/texture-tools
build/texture-tools/bin/pip install -r tools/recomp/texture-pack-requirements.txt
```

For an explicit development capture, launch the native app or smoke host
with `RECOMP_TEXTURE_DUMP_DIR` pointing at a writable directory. The renderer
writes each distinct source texture as RGBA PAM plus `textures.tsv`. This
capture performs disk I/O and is intentionally disabled during normal play.
Repeat captures for further levels into the same directory to extend coverage.

```sh
build/texture-tools/bin/python tools/recomp/texture_pack.py build \
  --capture build/hd-color/captured \
  --original original/gog/data/d3d \
  --output build/texture-pack
```

Compile the authored terrain material detail after building the pack:

```sh
build/texture-tools/bin/python tools/recomp/terrain_detail.py \
  --source games/populous/assets/terrain/materials-v1.png --output build/texture-pack
```

The initial pack only enlarged ground tiles from 16×16 to 64×64, which could
smooth pixels but could not add material structure. The new layer uses four
material channels stored at 1024×1024 (grass, sand, stone and soil) in one
shared texture. It costs about 5.3 MiB including all mip levels, counted within
the replacement budget, and one extra texture sample on applicable draws.
The source artwork and generation prompt are in `games/populous/assets/terrain/README.md`.

The shader preserves the original terrain's color and coastline masks while
adding zero-mean material structure before lighting and fog. Blue water fades
the land detail out. It applies only to complete, opaque 16/32-square RGB565
world tiles that write depth; Classic/replay, UI, sprite formats, partial atlas
UVs and mod-provider replacements retain their own textures. This layer also
leaves larger artist replacements (above 128×128) alone. It
works for uncaptured ground tiles, without allocating a replacement per tile.
It does not reconstruct higher-resolution height geometry or add new shore
shapes. Mip selection follows screen-space derivatives at the selected game
resolution, with anisotropic filtering and a neutral final mip to reduce
distant shimmer.

The builder matches captured pixels to their original PNG, including the
game's reduced-size copies, only when quantization explains the difference.
This recovers the original full-color source. The first level's sky had been
reduced from a 128×128 PNG to a 16×16 RGB565 texture; the pack restores that
source and expands it to 4096×4096. Clouds use 1024×1024. Other textures use a
bounded 4x enlargement. The changing 1024×1024 minimap atlas is skipped.

This is a faithful enlargement and color restoration, not newly painted
4K detail. The manifest distinguishes `restored-full-color` from
`enlarged-original`. The first local pack was built from a level-one gameplay
capture: 1,747 textures, six matched PNG sources, approximately 506 MiB on
disk. It covers captured terrain/water, buildings, units, effects and sky;
unseen textures and other campaign skies fall back to original assets.
A complete authored remaster would additionally require reviewed replacement
art for those assets.

An artist can replace any captured texture while preserving its aspect ratio
and atlas layout:

```sh
build/texture-tools/bin/python tools/recomp/texture_pack.py import \
  --reference build/hd-color/captured/0123456789abcdef.pam \
  --image /path/to/replacement.png --output build/texture-pack
```

The pack uses straight RGBA8. Offline resizing and mip generation filter
premultiplied colors, then restore straight alpha and bleed invisible edge
colors to avoid black fringes. `manifest.json` records provenance and SHA-256
for generated assets. The app builder copies the files listed by the manifest
into `Contents/Resources/texture-pack` before signing. Importing an image into
an existing pack updates its manifest and preload list.

## Runtime loading and memory

The default development directory is `build/texture-pack`; bundled apps use
their own resource directory. `RECOMP_TEXTURE_PACK_DIR` overrides either (an
empty value disables the pack). `RECOMP_TEXTURE_BUDGET_MB` accepts 32–1024 MiB;
the default replacement budget is 512 MiB, measured using Metal allocated sizes
including mipmaps and texture padding. It excludes
original textures and render targets.
Restart the app after changing pack files so its index and cached textures
are rebuilt. The in-game Textures option can switch the loaded pack on or off
without restarting.

The loader indexes and validates headers once. `preload.txt` prioritizes
textures before the first frame, up to 75% of the budget. Immutable GPU
textures are cached by content and shared across handles/revisions. Unleased,
completed resources are evicted in least-recently-used order; when active
resources fill the budget, an upload falls back to its original texture.
Files are revalidated at use. Texture and pitch limits are checked with
64-bit arithmetic before allocation. No resizing or image decoding happens
in a draw callback. Missing entries do not cause repeated filesystem probes.

Smoke reports HD draw, load, hit, refusal and resident-byte counters. These
prove whether replacements are used and bounded, not whether the display
sustains 120 FPS. A pinned-clock smoke run validates game behavior only.

## Public mod API

`PopModApi.texture_override_provider_ex` is an appended, size-gated v1 field;
all earlier offsets and original providers remain valid. Providers return a
`PopTextureReplacement` with `size`, dimensions, pitch, pixels and byte count.
Dimensions may differ from the source but must preserve aspect ratio and UV
layout. The limit is 4096 on either axis and 64 MiB of base-level RGBA storage.
Padded rows are supported. Provider storage remains valid until the next
invocation; the host copies it synchronously. Classic keeps a separate copy
of the original upload.

The original provider hash contract is unchanged. Native pack identity also
includes channel masks and color-key metadata so equal bytes in different
formats cannot alias. A shader still receives normalized UVs, regardless of
the physical replacement dimensions.

## Validation

Focused tests cover RGBA/24-bit four-byte storage, padded rows, non-RGB565
colors on the GPU, partial alpha, CPU/GPU surface upload equivalence, full-color
UI payloads, larger provider validation, ABI offsets, mip file validation,
HD/Classic/replay isolation, alpha fringes and source matching. Live validation
artifacts are recorded under `build/hd-color` in the implementation worktree.

The implementation passed the DirectX, host, mod and asset-tool suites. The
host suite includes real Metal pixel comparisons and a constrained-budget
fallback check; the mod suite includes saving and restoring the new controls
across a settings reset.

A native 3840×2160 level-one run recorded 1,097,235 HD world draws, 1,764
loads, 2,219 cache hits and zero refused replacements. Resident replacement
allocations were 535,723,904 bytes against a 536,870,912-byte budget. The
script selected a shaman, issued a movement command, observed 858 units of
movement and two state changes, captured the completed frame and exited
cleanly. This run used a pinned simulation clock and does not establish
sustained 4K120 performance. The packaged app passed strict code-signature
verification.

The terrain-detail follow-up passed GPU checks for land modulation, unchanged
water, Classic/legacy isolation, sprite/atlas exclusion, and larger authored
replacement exclusion. Asset tests cover channel independence, matched tile
edges, neutral distant mips and retaining the detail resource when a captured
pack is rebuilt. A 3840×2160 native gameplay run used material detail on
478,551 tile draws, with zero cache refusals and 535,674,752 resident bytes
including the detail resource. Selection, movement and clean exit passed.
Visual comparisons are under `build/hd-color/terrain-detail`; the new surface
structure is visible in close-up, while broad original color patches remain.
