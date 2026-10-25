import { readFile, writeFile, mkdir } from 'node:fs/promises';
import { resolve, dirname } from 'node:path';
import { createRequire } from 'node:module';
import { createHash } from 'node:crypto';
import { fileURLToPath } from 'node:url';

const argumentsByName = new Map();
for (let i = 2; i < process.argv.length; i += 2) {
  if (!process.argv[i].startsWith('--') || process.argv[i + 1] === undefined)
    throw new Error('Expected --source-directory, --output-directory and --sharp-module value pairs');
  argumentsByName.set(process.argv[i], process.argv[i + 1]);
}
const output = argumentsByName.get('--output-directory');
if (!output) throw new Error('An explicit leased --output-directory is required');
const parent = JSON.parse(process.env.PSX_WORK_CLEANUP_PARENT ?? 'null');
const outputDirectory = resolve(output);
if (!parent || !outputDirectory.toLowerCase().startsWith(resolve(parent.ProjectRoot, parent.WorkRoot).toLowerCase() + '\\'))
  throw new Error('Output must be a strict child of the wrapper cleanup lease');
const sourceDirectory = resolve(argumentsByName.get('--source-directory') ??
  resolve(dirname(fileURLToPath(import.meta.url)), '../source/shared/windows-icons/artwork'));
const require = createRequire(import.meta.url);
const sharp = require(argumentsByName.get('--sharp-module') ?? 'sharp');
const sizes = [16, 20, 24, 32, 40, 48, 64, 72, 96, 128, 256];
const hash = bytes => createHash('sha256').update(bytes).digest('hex').toUpperCase();

// 32-bit bottom-up DIB entries retain alpha; the AND mask also represents fully
// transparent pixels for native icon readers. Only 256 uses PNG compression.
function dib(rgba, size) {
  const maskStride = Math.ceil(size / 32) * 4;
  const imageBytes = size * size * 4;
  const bytes = Buffer.alloc(40 + imageBytes + maskStride * size);
  bytes.writeUInt32LE(40, 0);
  bytes.writeInt32LE(size, 4);
  bytes.writeInt32LE(size * 2, 8);
  bytes.writeUInt16LE(1, 12);
  bytes.writeUInt16LE(32, 14);
  bytes.writeUInt32LE(imageBytes + maskStride * size, 20);
  for (let y = 0; y < size; ++y) {
    const destinationY = size - 1 - y;
    for (let x = 0; x < size; ++x) {
      const source = (y * size + x) * 4;
      const destination = 40 + (destinationY * size + x) * 4;
      bytes[destination] = rgba[source + 2];
      bytes[destination + 1] = rgba[source + 1];
      bytes[destination + 2] = rgba[source];
      bytes[destination + 3] = rgba[source + 3];
      if (rgba[source + 3] === 0)
        bytes[40 + imageBytes + destinationY * maskStride + (x >> 3)] |= 0x80 >> (x & 7);
    }
  }
  return bytes;
}
function ico(frames) {
  const directory = Buffer.alloc(6 + frames.length * 16);
  directory.writeUInt16LE(1, 2);
  directory.writeUInt16LE(frames.length, 4);
  let offset = directory.length;
  frames.forEach(({ size, bytes }, i) => {
    const p = 6 + i * 16;
    directory[p] = directory[p + 1] = size === 256 ? 0 : size;
    directory.writeUInt16LE(1, p + 4);
    directory.writeUInt16LE(32, p + 6);
    directory.writeUInt32LE(bytes.length, p + 8);
    directory.writeUInt32LE(offset, p + 12);
    offset += bytes.length;
  });
  return Buffer.concat([directory, ...frames.map(frame => frame.bytes)]);
}

await mkdir(outputDirectory, { recursive: true });
const records = [];
const previews = [];
for (const [row, category] of ['CO', 'DU', 'HW', 'IOS'].entries()) {
  const svg = await readFile(resolve(sourceDirectory, `${category}.svg`));
  // Full SVG rendering preserves category vector paths over the embedded bitmap.
  const raster = await sharp(svg, { density: 288 }).resize(1024, 1024, {
    fit: 'contain', position: 'centre', background: { r: 0, g: 0, b: 0, alpha: 0 }
  }).ensureAlpha().png().toBuffer();
  const frames = [];
  let left = 16;
  for (const size of sizes) {
    const pipeline = sharp(raster).resize(size, size, { kernel: 'lanczos3' });
    const png = await pipeline.clone().png().toBuffer();
    const rgba = await pipeline.ensureAlpha().raw().toBuffer();
    frames.push({ size, bytes: size === 256 ? png : dib(rgba, size) });
    previews.push({ input: png, left, top: row * 288 + 16 });
    left += size + 12;
  }
  const bytes = ico(frames);
  await writeFile(resolve(outputDirectory, `${category}.ico`), bytes, { flag: 'wx' });
  records.push({ category, source: resolve(sourceDirectory, `${category}.svg`), sourceBytes: svg.length,
    sourceSHA256: hash(svg), bytes: bytes.length, sha256: hash(bytes), sizes });
}
await sharp({ create: { width: 986, height: 1152, channels: 4, background: '#707070' } })
  .composite(previews).png().toFile(resolve(outputDirectory, 'preview.png'));
await writeFile(resolve(outputDirectory, 'generation.json'), JSON.stringify({ schema: 1,
  renderer: sharp.versions, geometry: 'Complete SVG, aspect preserved, centered transparent square', records }, null, 2) + '\n', { flag: 'wx' });
console.log(JSON.stringify({ outputDirectory, records }));
