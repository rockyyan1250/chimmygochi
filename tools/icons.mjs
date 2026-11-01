// Renders the UI icons (flat SVG, 64x64 design grid) into LVGL TRUE_COLOR_ALPHA images:
// firmware/ChimmyGoChi/generated/icons.c + icons.h. Run automatically by tools/build.mjs.
import sharp from 'sharp';
import { mkdirSync, writeFileSync } from 'node:fs';

const O = '#4a3f4f', line = (w = 3) => `stroke="${O}" stroke-width="${w}" stroke-linejoin="round" stroke-linecap="round"`;
const art = {
  onigiri: `<path d="M32 7C39 7 58 40 58 48S52 58 32 58 6 56 6 48 25 7 32 7Z" fill="#fff" ${line()}/>
    <rect x="22" y="41" width="20" height="17" rx="2" fill="#2f3b3a"/>
    <circle cx="25" cy="32" r="2.5" fill="${O}"/><circle cx="39" cy="32" r="2.5" fill="${O}"/>
    <ellipse cx="19" cy="37" rx="3.5" ry="2" fill="#ffb3c1"/><ellipse cx="45" cy="37" rx="3.5" ry="2" fill="#ffb3c1"/>`,
  pudding: `<ellipse cx="32" cy="54" rx="26" ry="6" fill="#dfe9f5" ${line()}/>
    <path d="M16 52 20 24Q32 18 44 24L48 52Z" fill="#ffd96e" ${line()}/>
    <path d="M20 24Q32 18 44 24L43 31Q32 26 21 31Z" fill="#b5652c"/><circle cx="32" cy="15" r="5" fill="#ff5f6d" ${line(2.5)}/>`,
  strawberry: `<path d="M32 58C14 46 8 30 14 22S44 16 50 22 50 46 32 58Z" fill="#ff5f6d" ${line()}/>
    <path d="M20 20 32 11 44 20 38 23 32 18 26 23Z" fill="#5cc27a" ${line(2.5)}/>
    ${[[24,32],[40,32],[32,40],[22,42],[42,42],[32,50]].map(([x,y])=>`<ellipse cx="${x}" cy="${y}" rx="1.6" ry="2.4" fill="#fff3b0"/>`).join('')}`,
  apple: `<path d="M32 18C20 10 6 18 8 34S22 60 32 54C42 60 54 50 56 34S44 10 32 18Z" fill="#ff6b6b" ${line()}/>
    <path d="M32 18 33 8" ${line()}/><path d="M34 12Q44 4 48 10Q40 16 34 12Z" fill="#5cc27a" ${line(2.5)}/>
    <ellipse cx="20" cy="28" rx="4" ry="6" fill="#fff" opacity=".6"/>`,
  sandwich: `<path d="M8 50 32 12 56 50Z" fill="#ffe2a8" ${line()}/><path d="M16 42 32 18 48 42Z" fill="#fff7e6"/>
    <path d="M10 47q4-5 8 0t8 0 8 0 8 0 8 0 8 0" fill="none" stroke="#5cc27a" stroke-width="4" stroke-linecap="round"/>`,
  milk: `<path d="M18 22 32 10 46 22V56H18Z" fill="#fff" ${line()}/><path d="M18 22 32 10 46 22Z" fill="#cfe6ff" ${line()}/>
    <circle cx="32" cy="39" r="8" fill="#6bb8ff"/><path d="M28 39h8" stroke="#fff" stroke-width="3" stroke-linecap="round"/>`,
  ball: `<circle cx="32" cy="32" r="24" fill="#7cc8f0" ${line()}/><path d="M10 27Q32 38 54 27" fill="none" stroke="#fff" stroke-width="5"/>
    <ellipse cx="22" cy="18" rx="5" ry="3" fill="#fff" opacity=".7"/>`,
  bubbles: `<circle cx="24" cy="38" r="16" fill="#bfe9ff" fill-opacity=".8" stroke="#5aa9d6" stroke-width="3"/>
    <circle cx="45" cy="21" r="10" fill="#bfe9ff" fill-opacity=".8" stroke="#5aa9d6" stroke-width="3"/>
    <circle cx="47" cy="47" r="7" fill="#bfe9ff" fill-opacity=".8" stroke="#5aa9d6" stroke-width="3"/>
    <path d="M15 32q2-6 8-8M40 17q2-3 5-4" fill="none" stroke="#fff" stroke-width="3" stroke-linecap="round"/>`,
  picture: `<rect x="7" y="12" width="50" height="40" rx="7" fill="#fff" ${line()}/><rect x="13" y="18" width="38" height="28" rx="3" fill="#bfe6f5"/>
    <path d="M13 46 26 31 34 39 40 33 51 46Z" fill="#7fcf8e"/><circle cx="42" cy="25" r="4" fill="#ffd36b"/>`,
  paw: `<ellipse cx="32" cy="42" rx="13" ry="11" fill="#ffb3c1" ${line()}/>
    ${[[15,28],[25,17],[39,17],[49,28]].map(([x,y])=>`<circle cx="${x}" cy="${y}" r="6.5" fill="#ffb3c1" ${line()}/>`).join('')}`,
  house: `<path d="M16 30H48V54H16Z" fill="#ffe9b0" ${line()}/><path d="M8 32 32 10 56 32Z" fill="#ff8f8f" ${line()}/>
    <rect x="27" y="38" width="10" height="16" rx="4" fill="#d79a6a" ${line(2.5)}/>`,
  gear: `${[0,45,90,135,180,225,270,315].map(a=>`<rect x="27" y="5" width="10" height="14" rx="3" fill="#b8c4d6" ${line(2.5)} transform="rotate(${a} 32 32)"/>`).join('')}
    <circle cx="32" cy="32" r="18" fill="#b8c4d6" ${line()}/><circle cx="32" cy="32" r="7" fill="#fff" ${line()}/>`,
  moon: `<path d="M40 8A24 24 0 1 0 56 44 20 20 0 1 1 40 8Z" fill="#ffd96e" ${line()}/>
    <path d="M50 12l1.5 3.5 3.5 1.5-3.5 1.5L50 22l-1.5-3.5L45 17l3.5-1.5Z" fill="#fff4b1"/>`,
  sun: `${[0,45,90,135,180,225,270,315].map(a=>`<path d="M32 6V13" stroke="#ffb347" stroke-width="5" stroke-linecap="round" transform="rotate(${a} 32 32)"/>`).join('')}
    <circle cx="32" cy="32" r="13" fill="#ffc96b" ${line()}/>`,
  back: `<path d="M38 14 20 32 38 50" fill="none" ${line(7)}/>`,
  next: `<path d="M26 14 44 32 26 50" fill="none" ${line(7)}/>`,
  heart: `<path d="M32 54C10 40 6 28 12 18S28 12 32 20C36 12 46 10 52 18S54 40 32 54Z" fill="#ff7a93" ${line()}/>`,
  bolt: `<path d="M37 4 14 36H30L26 60 50 26H34Z" fill="#ffd23f" ${line()}/>`,
  plug: `<path d="M26 24V12M38 24V12" ${line(5)}/><rect x="18" y="22" width="28" height="20" rx="6" fill="#cfd8e3" ${line()}/><path d="M32 42V58" ${line(5)}/>`,
  speaker: `<path d="M10 24H20L34 12V52L20 40H10Z" fill="#cfd8e3" ${line()}/><path d="M42 22Q48 32 42 42M48 15Q59 32 48 49" fill="none" ${line(4)}/>`,
  screen: `<rect x="7" y="10" width="50" height="36" rx="7" fill="#3d4a5c" ${line()}/>
    <path d="M37 18A10 10 0 1 0 45 33 8 8 0 1 1 37 18Z" fill="#ffd96e"/><path d="M22 56H42M32 46V56" fill="none" ${line(4)}/>`,
  bellOff: `<path d="M17 44V30A15 15 0 0 1 47 30V44L51 49H13Z" fill="#ffd96e" ${line()}/><circle cx="32" cy="54" r="4" fill="${O}"/>
    <path d="M12 10 52 56" stroke="#ff6b6b" stroke-width="5" stroke-linecap="round"/>`,
  clock: `<circle cx="32" cy="32" r="24" fill="#fff" ${line()}/><path d="M32 18V32L42 38" fill="none" ${line(4)}/>`,
  power: `<path d="M21 17A20 20 0 1 0 43 17" fill="none" stroke="#3fb37f" stroke-width="6" stroke-linecap="round"/><path d="M32 8V30" stroke="#3fb37f" stroke-width="6" stroke-linecap="round"/>`,
  hourglass: `<path d="M20 10Q20 26 32 32 20 38 20 54H44Q44 38 32 32 44 26 44 10Z" fill="#fff4d6" ${line()}/>
    <path d="M24 52Q32 42 40 52Z" fill="#ffcf6b"/><path d="M16 8H48M16 56H48" ${line(4)}/>`,
  wifi: `<path d="M10 26Q32 6 54 26M18 35Q32 23 46 35M26 43Q32 38 38 43" fill="none" stroke="#5aa9d6" stroke-width="5" stroke-linecap="round"/>
    <circle cx="32" cy="51" r="4.5" fill="#5aa9d6"/>`,
  refresh: `<path d="M49 24A18 18 0 1 0 50 40" fill="none" ${line(5)}/><path d="M51 10V25H36" fill="none" ${line(5)}/>`,
  info: `<circle cx="32" cy="32" r="24" fill="#cfe6ff" ${line()}/><circle cx="32" cy="20" r="3.5" fill="${O}"/><path d="M32 29V46" ${line(5)}/>`,
  minus: `<path d="M14 32H50" ${line(8)}/>`,
  plus: `<path d="M14 32H50M32 14V50" ${line(8)}/>`,
  ok: `<path d="M14 34 27 47 51 19" fill="none" stroke="#3fb37f" stroke-width="8" stroke-linecap="round" stroke-linejoin="round"/>`,
};
// name -> [art, pixel size]
const icons = {
  nav_game: ['paw', 32], nav_home: ['house', 32], nav_settings: ['gear', 34],
  sleep: ['moon', 34], wake: ['sun', 34], back: ['back', 32],
  act_feed: ['onigiri', 38], act_play: ['ball', 38], act_room: ['picture', 38],
  food0: ['onigiri', 72], food1: ['pudding', 72], food2: ['strawberry', 72],
  food3: ['apple', 72], food4: ['sandwich', 72], food5: ['milk', 72],
  toy0: ['ball', 72], toy1: ['bubbles', 72], bubble: ['bubbles', 84],
  stat_food: ['onigiri', 20], stat_mood: ['heart', 20], stat_energy: ['bolt', 20],
  charge: ['bolt', 22], usb: ['plug', 22],
  set_bright: ['sun', 34], set_volume: ['speaker', 34], set_autooff: ['screen', 34], set_night: ['bellOff', 34],
  set_clock: ['clock', 34], set_start: ['power', 34], set_offline: ['hourglass', 34], set_sync: ['wifi', 34],
  set_reload: ['refresh', 34], set_info: ['info', 34], set_game: ['paw', 34], set_home: ['house', 34],
  minus: ['minus', 28], plus: ['plus', 28], prev: ['back', 28], next: ['next', 28], ok: ['ok', 34],
};

let c = '#include "lvgl.h"\n', h = '#pragma once\n#include "lvgl.h"\n';
const preview = [];
for (const [name, [key, size]] of Object.entries(icons)) {
  const svg = `<svg xmlns="http://www.w3.org/2000/svg" width="${size}" height="${size}" viewBox="0 0 64 64">${art[key]}</svg>`;
  const { data } = await sharp(Buffer.from(svg), { density: 72 * size / 64 * 4 }).resize(size, size)
    .ensureAlpha().raw().toBuffer({ resolveWithObject: true });
  preview.push({ input: Buffer.from(svg.replace(`width="${size}" height="${size}"`, 'width="64" height="64"')),
    left: (preview.length % 10) * 72 + 4, top: Math.floor(preview.length / 10) * 72 + 4 });
  const out = [];
  for (let i = 0; i < size * size; i++) {
    const [r, g, b, a] = data.subarray(i * 4, i * 4 + 4), v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
    out.push(v & 255, v >> 8, a);  // LV_IMG_CF_TRUE_COLOR_ALPHA, 16-bit, no byte swap
  }
  c += `static const uint8_t ${name}_map[] = {${out.join(',')}};\n` +
    `const lv_img_dsc_t icon_${name} = {{LV_IMG_CF_TRUE_COLOR_ALPHA, 0, 0, ${size}, ${size}}, ${out.length}, ${name}_map};\n`;
  h += `extern const lv_img_dsc_t icon_${name};\n`;
}
mkdirSync('firmware/ChimmyGoChi/generated', { recursive: true });
writeFileSync('firmware/ChimmyGoChi/generated/icons.c', c);
writeFileSync('firmware/ChimmyGoChi/generated/icons.h', h);
mkdirSync('build', { recursive: true });
await sharp({ create: { width: 720, height: Math.ceil(preview.length / 10) * 72, channels: 3, background: '#f5efe6' } })
  .composite(preview).png().toFile('build/icons-preview.png');
console.log(`Generated ${Object.keys(icons).length} icons`);
