// Collects every character used in KMB stop names, route ends and remarks into
// firmware/kmb_chars.txt, which tools/assets.mjs adds to the UI font so any stop can be shown.
// Re-run when KMB adds stops: node tools/kmb-chars.mjs
import { writeFileSync } from 'node:fs';
const K = 'https://data.etabus.gov.hk/v1/transport/kmb/';
const [stops, routes] = await Promise.all(['stop', 'route/'].map(u => fetch(K + u).then(r => r.json()).then(j => j.data)));
const remarks = '原定班次最後班次非實時資料暫停服務特別班次往由';
const set = new Set(remarks);
for (const s of stops) for (const ch of s.name_tc) set.add(ch);
for (const r of routes) for (const ch of r.orig_tc + r.dest_tc) set.add(ch);
const chars = [...set].filter(c => c.codePointAt(0) >= 0x20).sort().join('');
writeFileSync('firmware/kmb_chars.txt', chars + '\n');
console.log(`${[...chars].length} characters from ${stops.length} stops and ${routes.length} routes`);
