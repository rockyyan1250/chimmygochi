import sharp from 'sharp';
import { execFileSync } from 'node:child_process';
import { mkdirSync, writeFileSync, readFileSync, existsSync } from 'node:fs';
import { createHash } from 'node:crypto';
import { resolve, dirname } from 'node:path';

const source = resolve(process.argv[2] || 'assets/source/pack.json');
const output = resolve(process.argv[3] || 'assets/pack');
mkdirSync(output, { recursive: true });
mkdirSync('assets/source', { recursive: true });
mkdirSync('firmware/ChimmyGoChi/generated', { recursive: true });
const actions = ['idle','tap','eat','play','sleep','dress','refuse','happy'];
const palette = ['#6bd8d0','#ada4ed','#f5a8bf'];
function svg(pose, color) {
  const eyes = [2,6].includes(pose)
    ? '<path d="M92 124q8 8 16 0M148 124q8 8 16 0" fill="none" stroke="#263956" stroke-width="5" stroke-linecap="round"/>'
    : '<ellipse cx="100" cy="122" rx="5" ry="7" fill="#263956"/><ellipse cx="156" cy="122" rx="5" ry="7" fill="#263956"/>';
  return `<svg xmlns="http://www.w3.org/2000/svg" width="256" height="256" viewBox="0 0 256 256">
    <ellipse cx="128" cy="230" rx="65" ry="10" fill="#263956" opacity=".12"/>
    <g transform="translate(0 ${pose===1?-4:pose===4?-12:0})">
    <ellipse cx="87" cy="213" rx="25" ry="16" fill="${color}"/><ellipse cx="169" cy="213" rx="25" ry="16" fill="${color}"/>
    <path d="M56 126Q45 57 94 49Q126 28 158 49Q207 56 200 126L204 170Q201 219 128 218Q55 219 52 171Z" fill="${color}" stroke="#ffffff" stroke-width="5"/>
    <ellipse cx="128" cy="140" rx="59" ry="51" fill="#fffaf4"/>
    <ellipse cx="77" cy="143" rx="10" ry="6" fill="#f3a5b0" opacity=".75"/><ellipse cx="179" cy="143" rx="10" ry="6" fill="#f3a5b0" opacity=".75"/>
    ${eyes}
    ${pose===3?'<ellipse cx="128" cy="151" rx="8" ry="11" fill="#c36b80"/>':'<path d="M116 149q12 13 24 0" fill="none" stroke="#263956" stroke-width="4" stroke-linecap="round"/>'}
    <path d="M103 191h50" stroke="#fffaf4" stroke-width="9" stroke-linecap="round"/>
    ${pose===4?'<circle cx="206" cy="198" r="21" fill="#ffc76c"/><path d="M186 198h40M206 177v42" stroke="#fff" stroke-width="3"/>':''}
    ${pose===5?'<path d="m128 59 5 10 11 2-8 8 2 11-10-5-10 5 2-11-8-8 11-2Z" fill="#fff4b1"/>':''}
    ${pose===7?'<path d="m128 33-14-13a9 9 0 0 1 14-10 9 9 0 0 1 14 10Z" fill="#ff809b"/>':''}
    </g></svg>`;
}
function wav(notes) {
  const count=notes.length*2400, out=Buffer.alloc(44+count*2);
  out.write('RIFF'); out.writeUInt32LE(out.length-8,4); out.write('WAVEfmt ',8);
  out.writeUInt32LE(16,16); out.writeUInt16LE(1,20); out.writeUInt16LE(1,22);
  out.writeUInt32LE(16000,24); out.writeUInt32LE(32000,28); out.writeUInt16LE(2,32);
  out.writeUInt16LE(16,34); out.write('data',36); out.writeUInt32LE(count*2,40);
  for(let n=0;n<count;n++) {
    const t=n%2400, fade=Math.min(t/160,(2399-t)/300,1);
    out.writeInt16LE(Math.round(6500*fade*Math.sin(2*Math.PI*notes[Math.floor(n/2400)]*t/16000)),44+n*2);
  }
  return out;
}
if (!existsSync(source)) {
  if (process.argv[2]) throw Error(`Missing source manifest: ${source}`);
  const defs = { idle:[0,1,0,2], tap:[0,7,1,7], eat:[0,3,0,3], play:[0,4,1,4],
    sleep:[6,6,2,6], dress:[0,5,1,5], refuse:[2,0,2,0], happy:[7,1,7,0] };
  const pack = { schema:1, id:'neutral-demo-v1', placeholder:true, background:'room.png',
    outfits:palette.map((color,i)=>({name:['薄荷','薰衣草','蜜桃'][i],
      frames:Array.from({length:8},(_,p)=>`outfit${i}-${p}.png`)})),
    clips: Object.fromEntries(actions.map(a=>[a,{frames:defs[a],
      durations:a==='idle'?[500,300,600,120]:a==='sleep'?[600,600,150,600]:[160,180,180,250],
      loop:a==='idle'||a==='sleep',sound:a==='idle'||a==='sleep'?'':a==='eat'?'eat':a==='refuse'?'tap':'happy'}])),
    sounds:{tap:'tap.wav',happy:'happy.wav',eat:'eat.wav',dress:'dress.wav'} };
  for(let i=0;i<3;i++) for(let p=0;p<8;p++) await sharp(Buffer.from(svg(p,palette[i]))).png().toFile(`assets/source/outfit${i}-${p}.png`);
  const bg=`<svg xmlns="http://www.w3.org/2000/svg" width="480" height="480">
    <defs><linearGradient id="g" x2="0" y2="1"><stop stop-color="#f8f3ec"/><stop offset="1" stop-color="#e4efed"/></linearGradient></defs>
    <rect width="480" height="480" fill="url(#g)"/><rect y="282" width="480" height="198" fill="#e1d6c9"/>
    <path d="M0 282h480M0 330h480M0 390h480" stroke="#cbbdae" stroke-width="2"/>
    <rect x="30" y="95" width="92" height="118" rx="20" fill="#fff"/><rect x="38" y="103" width="76" height="102" rx="15" fill="#b8dce5"/>
    <path d="M76 104v100M39 154h74" stroke="#fff" stroke-width="6"/><circle cx="94" cy="125" r="12" fill="#fff2b0"/>
    <rect x="370" y="188" width="64" height="85" rx="10" fill="#d5aa8b"/>
    <path d="M402 189q-37-22-21-46q28 4 21 46q35-25 20-48q-25 3-20 48" fill="#81b499"/>
    <ellipse cx="240" cy="307" rx="113" ry="25" fill="#d0bbc8" opacity=".6"/></svg>`;
  await sharp(Buffer.from(bg)).png().toFile('assets/source/room.png');
  for(const [name,notes] of Object.entries({tap:[660],happy:[523,659,784],eat:[330,440],dress:[784,988]}))
    writeFileSync(`assets/source/${name}.wav`,wav(notes));
  writeFileSync(source,JSON.stringify(pack,null,2));
}
const input=JSON.parse(readFileSync(source));
if(input.schema!==1 || input.outfits?.length!==3) throw Error('schema=1 and exactly three outfits required');
const manifest={schema:1,id:input.id,placeholder:input.placeholder!==false,background:'room.cga',outfits:[],clips:input.clips,sounds:{},files:{}};
async function image(names, dest, bpp) {
  if(!names.length||names.length>12) throw Error('1..12 frames per atlas required');
  let w,h; const pixels=[];
  for(const name of names) {
    const {data,info}=await sharp(resolve(dirname(source),name)).ensureAlpha().raw().toBuffer({resolveWithObject:true});
    if(!w) {w=info.width;h=info.height;}
    if(w!==info.width||h!==info.height||(bpp===3?(w>320||h>320):(w!==480||h!==480)))
      throw Error('Atlas frames must match and be <=320px; background must be 480x480');
    const raw=Buffer.alloc(w*h*bpp);
    for(let i=0;i<w*h;i++) {
      raw.writeUInt16LE(((data[i*4]>>3)<<11)|((data[i*4+1]>>2)<<5)|(data[i*4+2]>>3),i*bpp);
      if(bpp===3) raw[i*bpp+2]=data[i*4+3];
    }
    pixels.push(raw);
  }
  const header=Buffer.alloc(16);header.write('CGA1');header.writeUInt16LE(w,4);header.writeUInt16LE(h,6);
  header.writeUInt16LE(names.length,8);header[10]=bpp;
  const data=Buffer.concat([header,...pixels]);
  if(data.length>3*1024*1024+16) throw Error('Atlas exceeds 3MiB');
  save(dest,data);
}
function save(name,data) {
  writeFileSync(`${output}/${name}`,data);
  manifest.files[name]={bytes:data.length,sha256:createHash('sha256').update(data).digest('hex')};
}
await image([input.background],'room.cga',2);
for(let i=0;i<3;i++) {
  const o=input.outfits[i];
  await image(o.frames,`outfit${i}.cga`,3);
  manifest.outfits.push({name:o.name,atlas:`outfit${i}.cga`});
}
for(const a of actions) {
  const c=input.clips[a];
  if(!c||!Array.isArray(c.frames)||c.frames.length<1||c.frames.length>32||
    c.frames.length!==c.durations?.length||c.durations.some(n=>!Number.isInteger(n)||n<16||n>5000)||
    c.frames.some(n=>!Number.isInteger(n)||n<0||input.outfits.some(o=>n>=o.frames.length))||
    typeof c.loop!=='boolean'||(c.sound&&!['tap','happy','eat','dress'].includes(c.sound)))
    throw Error(`Invalid clip ${a}`);
}
if(!input.sounds||['tap','happy','eat','dress'].some(id=>!input.sounds[id]))
  throw Error('All four sound slots are required');
for(const [id,file] of Object.entries(input.sounds)) {
  if(!['tap','happy','eat','dress'].includes(id)) throw Error('Unsupported sound slot');
  const data=readFileSync(resolve(dirname(source),file));
  if(data.length<44||data.length>160044||data.toString('ascii',0,4)!=='RIFF'||
    data.toString('ascii',8,16)!=='WAVEfmt '||data.readUInt32LE(16)!==16||
    data.readUInt16LE(20)!==1||data.readUInt16LE(22)!==1||data.readUInt32LE(24)!==16000||
    data.readUInt16LE(34)!==16||data.toString('ascii',36,40)!=='data'||
    data.readUInt32LE(40)!==data.length-44||(data.length-44)%2||
    data.readUInt32LE(4)!==data.length-8||data.readUInt32LE(28)!==32000||data.readUInt16LE(32)!==2)
    throw Error(`Use canonical 16kHz mono 16bit PCM WAV: ${file}`);
  save(`${id}.wav`,data); manifest.sounds[id]=`${id}.wav`;
}
writeFileSync(`${output}/manifest.json`,JSON.stringify(manifest,null,2));
mkdirSync('.deps/fonts',{recursive:true});
if(!existsSync('.deps/fonts/NotoSansCJKtc-Regular.otf')) {
  execFileSync('curl',['-fL','--retry','2','-o','.deps/fonts/NotoSansCJKtc-Regular.otf',
    'https://raw.githubusercontent.com/notofonts/noto-cjk/Sans2.004/Sans/OTF/TraditionalChinese/NotoSansCJKtc-Regular.otf'],{stdio:'inherit'});
}
if(!existsSync('.deps/fonts/OFL.txt')) execFileSync('curl',['-fL','-o','.deps/fonts/OFL.txt',
  'https://raw.githubusercontent.com/notofonts/noto-cjk/Sans2.004/LICENSE'],{stdio:'inherit'});
const text=readFileSync('firmware/ChimmyGoChi/UiText.h','utf8')+
  readFileSync('firmware/ChimmyGoChi/App.cpp','utf8')+JSON.stringify(input)+
  '一二三四五六日年月星期上午下午零0123456789:-/%';
execFileSync(process.execPath,['node_modules/lv_font_conv/lv_font_conv.js','--font','.deps/fonts/NotoSansCJKtc-Regular.otf',
  '--size','24','--bpp','4','--format','lvgl','--no-compress','--symbols',Array.from(new Set(text.replace(/[\x00-\x1f]/g,''))).join(''),
  '--lv-font-name','font_tc','--output','firmware/ChimmyGoChi/generated/font_tc.c'],{stdio:'inherit'});
execFileSync(process.execPath,['node_modules/lv_font_conv/lv_font_conv.js','--font','.deps/fonts/NotoSansCJKtc-Regular.otf',
  '--size','88','--bpp','4','--format','lvgl','--no-compress','--symbols','0123456789:-',
  '--lv-font-name','font_clock','--output','firmware/ChimmyGoChi/generated/font_clock.c'],{stdio:'inherit'});
console.log(`Built ${Object.keys(manifest.files).length} replaceable files in ${output}`);
