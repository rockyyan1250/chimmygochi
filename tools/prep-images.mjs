// Normalizes generated character PNGs (any size, white or transparent background) into
// 256x256 transparent frames with a shared scale and foot line, then writes a pack.json
// for tools/assets.mjs. Usage: node tools/prep-images.mjs [assets/chimmy]
import sharp from 'sharp';
import { existsSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { resolve, join, relative } from 'node:path';

const root = resolve(process.argv[2] || 'assets/chimmy');
const configPath = join(root, 'chimmy.json');
const SIZE = 256, HEIGHT = 192, FOOT = 224, CENTER = 128;  // 75% tall, feet at 87.5%
const actions = ['idle','tap','eat','play','sleep','dress','refuse','happy'];
mkdirSync(join(root, 'raw'), { recursive: true });
if (!existsSync(configPath)) {
  writeFileSync(configPath, JSON.stringify({
    reference: 'stand_normal.png',
    outfits: [{ name: '原本', dir: 'raw' }, { name: '原本', dir: 'raw' }, { name: '原本', dir: 'raw' }],
    // mode: "fit" matches the reference height and foot line (expression variants);
    // "canvas" keeps the pose's position/size relative to the generator's canvas (jump, sleep).
    // scale/dx/dy nudge a frame after placement; holes are [x,y] source pixels of enclosed background.
    frames: { 'jump.png': { mode: 'canvas' }, 'sleep_curl.png': { mode: 'canvas' } },
    clips: {
      idle: { frames: ['stand_normal.png','stand_blink_closed.png'], durations: [2600,150], loop: true, sound: '' },
      tap: { frames: ['stand_curious.png','stand_normal.png'], durations: [500,200], loop: false, sound: 'tap' },
      eat: { frames: ['food_ready.png','stand_satisfied.png','food_ready.png','stand_satisfied.png'], durations: [300,300,300,600], loop: false, sound: 'eat' },
      play: { frames: ['hold_ball.png','jump.png','hold_ball.png','jump.png'], durations: [300,250,300,350], loop: false, sound: 'happy' },
      sleep: { frames: ['sleep_curl.png'], durations: [1000], loop: true, sound: '' },
      dress: { frames: ['wave.png','stand_happy.png','wave.png'], durations: [300,300,500], loop: false, sound: 'dress' },
      refuse: { frames: ['stand_refuse.png','stand_normal.png'], durations: [600,200], loop: false, sound: 'tap' },
      happy: { frames: ['stand_happy.png','cheer.png','stand_happy.png','cheer.png'], durations: [250,300,250,450], loop: false, sound: 'happy' },
    },
    placeholder: true,
    background: '../source/room.png',
    sounds: { tap: '../source/tap.wav', happy: '../source/happy.wav', eat: '../source/eat.wav', dress: '../source/dress.wav' },
  }, null, 2));
  console.log(`Created ${configPath}. Put generated PNGs in ${join(root, 'raw')} and run again.`);
  process.exit(0);
}
const config = JSON.parse(readFileSync(configPath, 'utf8'));
if (config.outfits?.length !== 3) throw Error('chimmy.json: exactly three outfits required (they may share a dir)');

// Flood-fills the background from the image border so white face/hands inside the outline stay.
function removeBackground(data, w, h, holes = []) {
  const at = i => i * 4, cornerAlpha = [0, w - 1, (h - 1) * w, h * w - 1].map(i => data[at(i) + 3]);
  if (Math.min(...cornerAlpha) < 250) return;  // already transparent
  const bg = [0, 0, 0]; let n = 0;
  for (const [cx, cy] of [[0,0],[w-8,0],[0,h-8],[w-8,h-8]])
    for (let y = cy; y < cy + 8; y++) for (let x = cx; x < cx + 8; x++) {
      for (let c = 0; c < 3; c++) bg[c] += data[at(y * w + x) + c]; n++;
    }
  for (let c = 0; c < 3; c++) bg[c] /= n;
  const dist = i => Math.hypot(data[at(i)] - bg[0], data[at(i) + 1] - bg[1], data[at(i) + 2] - bg[2]);
  const NEAR = 60, FAR = 160, isBg = new Uint8Array(w * h), queue = [];
  const seed = i => { if (!isBg[i] && dist(i) < NEAR) { isBg[i] = 1; queue.push(i); } };
  for (let x = 0; x < w; x++) { seed(x); seed((h - 1) * w + x); }
  for (let y = 0; y < h; y++) { seed(y * w); seed(y * w + w - 1); }
  for (const [x, y] of holes) seed(Math.round(y) * w + Math.round(x));
  while (queue.length) {
    const i = queue.pop(), x = i % w;
    if (x > 0) seed(i - 1); if (x < w - 1) seed(i + 1);
    if (i >= w) seed(i - w); if (i < w * (h - 1)) seed(i + w);
  }
  for (let i = 0; i < w * h; i++) {
    if (isBg[i]) { data[at(i) + 3] = 0; continue; }
    const x = i % w;
    const edge = (x > 0 && isBg[i - 1]) || (x < w - 1 && isBg[i + 1]) || (i >= w && isBg[i - w]) || (i < w * (h - 1) && isBg[i + w]);
    if (!edge) continue;
    // Soften the 1px fringe and remove the background tint from its colour.
    const a = Math.min(1, Math.max(0, (dist(i) - NEAR) / (FAR - NEAR)));
    for (let c = 0; c < 3; c++) data[at(i) + c] = a > 0.05 ? Math.min(255, Math.max(0, Math.round((data[at(i) + c] - bg[c] * (1 - a)) / a))) : 0;
    data[at(i) + 3] = Math.round(data[at(i) + 3] * a);
  }
}
async function load(file, holes) {
  const { data, info } = await sharp(file).ensureAlpha().raw().toBuffer({ resolveWithObject: true });
  const w = info.width, h = info.height;
  removeBackground(data, w, h, holes);
  let x0 = w, y0 = h, x1 = -1, y1 = -1;
  for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) if (data[(y * w + x) * 4 + 3] > 32) {
    if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y;
  }
  if (x1 < 0) throw Error(`${file}: no character found after background removal`);
  return { data, w, h, box: { x0, y0, x1: x1 + 1, y1: y1 + 1 } };
}

const raws = config.outfits.map(o => resolve(root, o.dir));
const ref = await load(join(raws[0], config.reference), config.frames?.[config.reference]?.holes);
const refScale = HEIGHT / (ref.box.y1 - ref.box.y0);
const refU = (ref.box.x0 + ref.box.x1) / 2 / ref.w, refV = ref.box.y1 / ref.h;

// Frames: the reference first, then every existing file the clips use (missing ones fall back).
const names = [config.reference], missing = new Set();
for (const a of actions) {
  const clip = config.clips?.[a];
  if (!clip) throw Error(`chimmy.json: clip "${a}" missing`);
  for (const f of clip.frames) {
    if (names.includes(f) || missing.has(f)) continue;
    if (raws.every(dir => existsSync(join(dir, f)))) names.push(f); else missing.add(f);
  }
}
if (names.length > 12) throw Error(`${names.length} frames used; an outfit atlas holds at most 12: ${names.join(', ')}`);
if (missing.size) console.warn(`Not found yet, using ${config.reference} instead: ${[...missing].join(', ')}`);

const outDir = join(root, 'frames'), outfits = [], sheet = [];
mkdirSync(outDir, { recursive: true });
for (let o = 0; o < 3; o++) {
  const frames = [];
  for (const [index, name] of names.entries()) {
    const opt = config.frames?.[name] || {};
    const img = await load(join(raws[o], name), opt.holes);
    const { x0, y0, x1, y1 } = img.box, bw = x1 - x0, bh = y1 - y0;
    const mode = opt.mode || 'fit';
    let s, left, top;
    if (mode === 'fit') {
      s = HEIGHT / bh * (opt.scale || 1);
      left = CENTER - bw * s / 2; top = FOOT - bh * s;
    } else if (mode === 'canvas') {
      // Same "camera" as the reference: the generator's canvas maps onto ours identically.
      const k = Math.sqrt(ref.w * ref.h / (img.w * img.h));
      s = refScale * k * (opt.scale || 1);
      left = CENTER + (x0 / img.w - refU) * ref.w * refScale; top = FOOT + (y0 / img.h - refV) * ref.h * refScale;
    } else throw Error(`${name}: mode must be "fit" or "canvas"`);
    left = Math.round(left + (opt.dx || 0)); top = Math.round(top + (opt.dy || 0));
    const tw = Math.max(1, Math.round(bw * s)), th = Math.max(1, Math.round(bh * s));
    const scaled = await sharp(img.data, { raw: { width: img.w, height: img.h, channels: 4 } })
      .extract({ left: x0, top: y0, width: bw, height: bh })
      .resize(tw, th, { kernel: 'lanczos3', fit: 'fill' }).raw().toBuffer();
    const canvas = Buffer.alloc(SIZE * SIZE * 4);
    let clipped = false;
    for (let y = 0; y < th; y++) for (let x = 0; x < tw; x++) {
      const cx = left + x, cy = top + y, src = (y * tw + x) * 4;
      if (cx < 0 || cy < 0 || cx >= SIZE || cy >= SIZE) { if (scaled[src + 3] > 32) clipped = true; continue; }
      scaled.copy(canvas, (cy * SIZE + cx) * 4, src, src + 4);
    }
    if (clipped) console.warn(`${config.outfits[o].name}/${name}: cut off at the 256px edge; adjust scale/dx/dy`);
    const file = `outfit${o}-${String(index).padStart(2, '0')}-${name.replace(/\.png$/i, '')}.png`;
    await sharp(canvas, { raw: { width: SIZE, height: SIZE, channels: 4 } }).png().toFile(join(outDir, file));
    frames.push(`frames/${file}`);
    if (o === 0) sheet.push({ file: join(outDir, file), name });
  }
  outfits.push({ name: config.outfits[o].name, frames });
}

const clips = {};
for (const a of actions) {
  const c = config.clips[a];
  clips[a] = { ...c, frames: c.frames.map(f => names.indexOf(missing.has(f) ? config.reference : f)) };
}
const rel = p => relative(root, resolve(root, p)).replaceAll('\\', '/');
writeFileSync(join(root, 'pack.json'), JSON.stringify({
  schema: 1, id: config.id || 'chimmy-v1', placeholder: config.placeholder !== false,
  background: rel(config.background), outfits, clips,
  sounds: Object.fromEntries(Object.entries(config.sounds).map(([k, v]) => [k, rel(v)])),
}, null, 2));

// Contact sheet: red = foot line, blue = centre, to spot frames that jump between poses.
const cols = 4, rows = Math.ceil(sheet.length / cols), cell = SIZE + 8;
const grid = Buffer.from(`<svg xmlns="http://www.w3.org/2000/svg" width="${cols * cell}" height="${rows * (cell + 20)}">
  <rect width="100%" height="100%" fill="#e8e2d8"/>${sheet.map((f, i) => {
    const x = (i % cols) * cell + 4, y = Math.floor(i / cols) * (cell + 20) + 4;
    return `<rect x="${x}" y="${y}" width="${SIZE}" height="${SIZE}" fill="#f8f3ec"/>
      <line x1="${x}" x2="${x + SIZE}" y1="${y + FOOT}" y2="${y + FOOT}" stroke="#d33" stroke-width="1"/>
      <line x1="${x + CENTER}" x2="${x + CENTER}" y1="${y}" y2="${y + SIZE}" stroke="#36c" stroke-width="1" stroke-dasharray="4 4"/>
      <text x="${x}" y="${y + SIZE + 15}" font-family="sans-serif" font-size="13" fill="#333">${i}: ${f.name}</text>`;
  }).join('')}</svg>`);
await sharp(grid).composite(sheet.map((f, i) => ({ input: f.file,
  left: (i % cols) * cell + 4, top: Math.floor(i / cols) * (cell + 20) + 4 }))).png().toFile(join(root, 'preview.png'));
console.log(`${names.length} frames x 3 outfits -> ${join(root, 'pack.json')}; check ${join(root, 'preview.png')}`);
console.log(`Next: node tools/assets.mjs ${relative(process.cwd(), join(root, 'pack.json'))} assets/pack`);
