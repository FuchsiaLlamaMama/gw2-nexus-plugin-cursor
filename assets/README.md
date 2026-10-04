# Cursor Finder preset art

Layered preset art for the five cursor presets (the layered recolor
mechanism). Rasterized from the vector design sources in [`./svg`](svg/); no game
textures are bundled.

## Layer scheme

Each preset is composited from independently-tintable **white / alpha masks** —
none has a signature hue baked in. At draw time the addon tints each layer with
an `ImGui` `AddImage` colour, so the same PNG serves any user colour.

| Layer | File | Tinted by | Default |
|-------|------|-----------|---------|
| Outline | `<preset>-outline.png` | Outline colour | on, dark `#111111`-ish |
| Colour  | `<preset>-colour.png`  | Colour                | the preset's signature hue |

**Draw order: outline *under* colour.** The design sources draw the dark outline
first and the coloured core on top, so the outline reads as a thin dark halo
around the brighter core. Drawing colour first buries it under the wider outline
stroke — get this order right at the draw site.

Source alphas from the design are preserved in the masks (the 0.9 dark-outline
opacity; the Soft Halo radial gradient), so the default render matches the design
without extra per-layer opacity.

### Signature hues (colour-layer defaults)

| Preset | Slug | Hue |
|--------|------|-----|
| Pulse Ring (default) | `pulse-ring` | magenta `#ff2d9b` |
| Corner Reticle | `corner-reticle` | cyan `#22e0ff` |
| Beacon Crosshair | `beacon-crosshair` | white `#f2f2f6` |
| Radar Dash | `radar-dash` | violet `#b26bff` |
| Soft Halo | `soft-halo` | teal `#3fd4c9` |

## Not yet generated

- **Fill layer.** The design has no fill geometry, so no fill mask is
  authored here. The fill centre is expected to be a procedural translucent disc
  (or a shared soft-disc mask) tinted by Fill colour — an implementation choice
  in code, not invented art. Off by default.
- **Soft Halo outline.** `soft-halo-outline.png` carries the design's *light* inner
  ring (`#eafffb`) as a mask; tinting it with the global dark default inverts the
  design intent. Left open (per-preset outline default, or outline-off for
  Soft Halo).

## Regenerate (repeatable)

The PNGs are rendered from `./svg/*.svg` with the resvg engine at 256×256, then
embedded into the DLL as C byte arrays (the addon loads them via
`Textures_GetOrCreateFromMemory` — a `.rc`/`FindResource` path was tried first
but `FindResource` by numeric ID failed at runtime, so the bytes are compiled in
directly):

Run from this `assets/` folder:

```bash
npm install @resvg/resvg-js
node rasterize.mjs            # svg/*.svg -> presets/*.png (256x256)
python3 gen_texture_data.py  # presets/*.png -> preset_textures_data.h (byte arrays)
```

Editing a mask means re-running both steps. `node_modules` is not committed;
`preset_textures_data.h` is (it's the source of truth the DLL compiles in).
