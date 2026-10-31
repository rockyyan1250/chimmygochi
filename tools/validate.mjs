import { readFileSync, readdirSync } from 'node:fs';
import { createHash } from 'node:crypto';
import { execFileSync } from 'node:child_process';
const dir=process.argv[2]||'assets/pack';
const m=JSON.parse(readFileSync(`${dir}/manifest.json`));
if(m.schema!==1||m.outfits.length!==3) throw Error('Invalid manifest');
for(const [name,meta] of Object.entries(m.files)) {
  if(!/^[a-z0-9_.-]{1,48}$/.test(name)||name.includes('..')) throw Error('Unsafe filename');
  const b=readFileSync(`${dir}/${name}`);
  if(b.length!==meta.bytes||createHash('sha256').update(b).digest('hex')!==meta.sha256) throw Error(`Checksum mismatch: ${name}`);
  if(name.endsWith('.cga')) {
    const w=b.readUInt16LE(4),h=b.readUInt16LE(6),n=b.readUInt16LE(8),bpp=b[10];
    if(b.toString('ascii',0,4)!=='CGA1'||w>480||h>480||!w||!h||n>12||!n||
       ![2,3].includes(bpp)||b.length!==16+w*h*n*bpp) throw Error(`Invalid image ${name}`);
  }
}
for(const f of readdirSync('tools').filter(f=>f.endsWith('.mjs')))
  execFileSync(process.execPath,['--check',`tools/${f}`]);
console.log('Pack hashes, image sizes, manifest and JavaScript syntax validated.');
