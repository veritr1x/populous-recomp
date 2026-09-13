# Terrain material source

`materials-v1.png` is newly generated material artwork for the native Enhanced
renderer. It contains grass, sand, stone and soil quadrants. It was produced
with the built-in image generation tool on 2026-09-10, without original game
images as input. It adds surface structure; it does not replace campaign
colors, coastline shapes or baked lighting.
The returned atlas is 1254×1254 (627×627 per source material); the runtime data
texture stores each material channel at 1024×1024 with a complete mip chain.

Compile it with:

```sh
.venv/bin/python tools/recomp/terrain_detail.py \
  --source games/populous/assets/terrain/materials-v1.png --output build/texture-pack
```

The compiler removes broad shading, joins tile edges and packs four independent
material channels into one mipmapped RGBA data texture. Its alpha channel is
soil data, not transparency. The final mip is neutral. The runtime's 512 MiB
replacement budget includes this resource.

Generation prompt (built-in tool):

```
Use case: stylized-concept
Asset type: production game terrain material texture atlas, square 2048x2048 pixels. Primary request: exactly four equally sized square material textures in a strict 2 by 2 grid, each fills its own quadrant right to its edges, no margins, no lines separating them, no labels. These will supply fine detail for an HD restoration of an old fantasy strategy game, keeping its hand-painted earthy style. Top left: dense short fine grass blades and very small moss tufts, low contrast olive meadow, entirely ground, no flowers. Top right: fine granular sandy earth, small varied grains and tiny worn pebbles, muted beige, no large objects. Bottom left: gently weathered natural stone, fine small mineral structure and subtle irregular hairline fissures, neutral gray, no large dramatic cracks. Bottom right: fine compact earth with subtle crumbly clods and tiny particles, warm muted brown, no plant litter. Every quadrant must be a spatially uniform, seamless repeating top-down flat albedo material: orthographic directly overhead, evenly lit, no directional light, no cast shadows, no vignette, no perspective or horizon. Detail should be crisp and fine, visible at full resolution, neither blurry nor pixelated; coherent natural material structure, not digital random speckle. No landscape composition, no objects, no buildings, no water, no text, no watermark. Each of the four materials independently tiles at all four edges. Preserve moderate contrast and avoid broad light/dark patches.
```
