// Prepares public/ from the repo, so the site and the README share one set of images:
// cuts the single handhelds out of the docs/images strips, and turns the launcher's system drawings and mark
// into web images.
import { copyFile, mkdir, readFile, rm, writeFile } from 'node:fs/promises';
import { existsSync } from 'node:fs';
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
  'second-screen.png': { width: 1920, height: 514, names: ['pokemon', 'sma4', 'circuit', 'mk64'] },
  'library.png': { width: 1440, height: 514, names: ['cartridges', 'covers', 'list'] },
};
// The systems index.astro lists, in its order.
const systems = ['nds', '3ds', 'gba', 'gb', 'gbc', 'nes', 'snes', 'md', 'sms', 'pce', 'psx', 'psp', 'n64', 'atari', 'coleco',
  'intellivision', 'zx', 'c64', 'msx', 'amiga', 'pico8', 'gw', 'scummvm', 'ports'];
const shot = (name) => join(pub, 'shots', `${name}.webp`);

await mkdir(join(pub, 'shots'), { recursive: true });
await rm(join(pub, 'systems'), { recursive: true, force: true });
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

// Full-size handheld renders (1384x1471, backdrop around the device): trim to the handheld.
const renders = { home: 'site-home.webp', systems: 'site-systems.webp' };
await Promise.all(Object.entries(renders).map(async ([name, file]) => {
  const path = join(repo, 'docs/images', file);
  const { width, height } = await sharp(path).metadata();
  if (width !== 1384 || height !== 1471) {
    throw new Error(`docs/images/${file} is ${width}x${height}, expected 1384x1471.`);
  }
  await sharp(path)
    .extract({ left: 104, top: 104, width: 1176, height: 1264 })
    .resize({ width: 1168 })
    .webp({ quality: 88 })
    .toFile(shot(name));
}));

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

// A system's drawing as the launcher's Rich icon style shows it (assets/systems/<id>.bmp: 128x128 32-bit BGRA,
// tools/prepare_system_icons.py), else its 16x16 pixel glyph, which is what the launcher falls back to as well.
async function richIcon(bmp, out) {
  const file = await readFile(bmp);
  const offset = file.readUInt32LE(10), width = file.readInt32LE(18), rows = file.readInt32LE(22), bits = file.readUInt16LE(28);
  if (bits !== 32) throw new Error(`${bmp} is ${bits}-bit, expected 32-bit BGRA.`);
  const height = Math.abs(rows);
  const rgba = Buffer.alloc(width * height * 4);
  for (let y = 0; y < height; y++) {
    const from = offset + (rows < 0 ? y : height - 1 - y) * width * 4; // a positive height is stored bottom-up
    for (let x = 0; x < width; x++) {
      const i = from + x * 4, o = (y * width + x) * 4;
      rgba[o] = file[i + 2]; rgba[o + 1] = file[i + 1]; rgba[o + 2] = file[i]; rgba[o + 3] = file[i + 3];
    }
  }
  await sharp(rgba, { raw: { width, height, channels: 4 } }).webp({ quality: 90, alphaQuality: 100 }).toFile(out);
}
const icons = Object.fromEntries(systems.map((s) => [s, existsSync(join(repo, 'assets/systems', `${s}.bmp`)) ? 'webp' : 'svg']));
await Promise.all(systems.map((s) => icons[s] === 'webp'
  ? richIcon(join(repo, 'assets/systems', `${s}.bmp`), join(pub, 'systems', `${s}.webp`))
  : copyFile(join(repo, 'assets/systems/pixel', `${s}.svg`), join(pub, 'systems', `${s}.svg`))));
// Which file each system got, for index.astro.
await mkdir(join(site, 'src/generated'), { recursive: true });
await writeFile(join(site, 'src/generated/system-icons.json'), JSON.stringify(icons, null, 2) + '\n');
await copyFile(join(repo, 'assets/brand/mark.svg'), join(pub, 'favicon.svg'));
