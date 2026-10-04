// Rasterize the Cursor Finder preset layer masks (SVG -> 256x256 PNG).
//
// The five presets are authored as WHITE / ALPHA masks under ./svg — one
// file per layer (`<preset>-outline.svg`, `<preset>-colour.svg`). "White mask"
// means every layer is drawn in #ffffff so the addon can tint it at draw time
// with ImGui's AddImage colour; no signature hue is baked
// in. Source alphas from the design (e.g. the 0.9 dark-outline opacity, the
// Soft Halo gradient) ARE preserved in the mask so the default render matches the
// design.
//
// Engine: @resvg/resvg-js (the resvg renderer). Deterministic, no browser.
//
// Regenerate (from the assets/ folder):
//   npm install @resvg/resvg-js
//   node rasterize.mjs
//
// Every *.svg in ./svg is rendered to ./presets/<same-name>.png at OUT_SIZE.

import { readdirSync, readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import { dirname, join, basename } from 'node:path';
import { fileURLToPath } from 'node:url';
import { Resvg } from '@resvg/resvg-js';

const HERE = dirname(fileURLToPath(import.meta.url));
const SRC = join(HERE, 'svg');
const OUT = join(HERE, 'presets');
const OUT_SIZE = 256; // px, square (design viewBox is 200x200)

mkdirSync(OUT, { recursive: true });

const svgs = readdirSync(SRC).filter((f) => f.endsWith('.svg')).sort();
if (svgs.length === 0) {
  console.error(`No .svg files found in ${SRC}`);
  process.exit(1);
}

for (const file of svgs) {
  const svg = readFileSync(join(SRC, file), 'utf8');
  const resvg = new Resvg(svg, {
    fitTo: { mode: 'width', value: OUT_SIZE },
    background: 'rgba(0,0,0,0)', // transparent; only the mask carries alpha
  });
  const png = resvg.render().asPng();
  const out = join(OUT, `${basename(file, '.svg')}.png`);
  writeFileSync(out, png);
  console.log(`${file}  ->  presets/${basename(file, '.svg')}.png  (${png.length} B)`);
}

console.log(`\nDone: ${svgs.length} PNG(s) at ${OUT_SIZE}x${OUT_SIZE}.`);
