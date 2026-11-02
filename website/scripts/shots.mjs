// Prepares public/ from the repo, so the site and the README share one set of images:
// cuts the single handhelds out of the docs/images strips and copies the mark and system icons.
import { copyFile, mkdir } from 'node:fs/promises';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import sharp from 'sharp';

const site = join(dirname(fileURLToPath(import.meta.url)), '..');
const repo = join(site, '..');
const pub = join(site, 'public');

// One name per handheld, left to right (null = not used on the site). index.astro captions
// these names, so a strip of another size stops the build: check its order and captions, then
// update the size here and the image sizes in index.astro.
const strips = {
  'screens.png': { width: 1800, height: 643, names: ['home', null, 'pause'] },
  'second-screen.png': { width: 1920, height: 514, names: ['pokemon', 'sma4', 'circuit', 'mk64'] },
  'library.png': { width: 1440, height: 514, names: ['cartridges', 'covers', 'list'] },
};
const systems = ['nds', '3ds', 'gba', 'gb', 'gbc', 'nes', 'snes', 'md', 'sms', 'psx', 'psp', 'n64', 'zx', 'c64', 'ports'];
const shot = (name) => join(pub, 'shots', `${name}.webp`);

await mkdir(join(pub, 'shots'), { recursive: true });
await mkdir(join(pub, 'systems'), { recursive: true });

for (const [file, { width: expectedWidth, height: expectedHeight, names }] of Object.entries(strips)) {
  const path = join(repo, 'docs/images', file);
  const { width, height } = await sharp(path).metadata();
  if (width !== expectedWidth || height !== expectedHeight) {
    throw new Error(`docs/images/${file} is ${width}x${height}, expected ${expectedWidth}x${expectedHeight}. See the note in scripts/shots.mjs.`);
  }
  const cell = Math.floor(width / names.length);
  // Trim the strip's own backdrop down to the handheld's edges.
  const side = Math.round(cell * 8 / 600);
  const top = side;
  const bottom = Math.round(side / 2);
  await Promise.all(names.map((name, i) => name && sharp(path)
    .extract({ left: i * cell + side, top, width: cell - 2 * side, height: height - top - bottom })
    .webp({ quality: 88 })
    .toFile(shot(name))));
}

// Link preview for social sites: 1200x630 PNG, which they all read.
const [home, circuit] = await Promise.all(['home', 'circuit'].map((name) =>
  sharp(shot(name)).resize({ height: 590 }).png().toBuffer({ resolveWithObject: true })));
const gap = 36;
const left = Math.round((1200 - home.info.width - circuit.info.width - gap) / 2);
await sharp({ create: { width: 1200, height: 630, channels: 3, background: '#0c111a' } })
  .composite([
    { input: home.data, left, top: 20 },
    { input: circuit.data, left: left + home.info.width + gap, top: 20 },
  ])
  .png({ palette: true })
  .toFile(join(pub, 'og.png'));

await Promise.all(systems.map((s) =>
  copyFile(join(repo, 'assets/systems/pixel', `${s}.svg`), join(pub, 'systems', `${s}.svg`))));
await copyFile(join(repo, 'assets/brand/mark.svg'), join(pub, 'favicon.svg'));
