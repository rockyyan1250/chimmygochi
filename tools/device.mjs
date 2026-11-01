import { SerialPort } from 'serialport';
import { readFileSync, writeFileSync } from 'node:fs';
import { createHash } from 'node:crypto';
import { once } from 'node:events';
import { StringDecoder } from 'node:string_decoder';
import sharp from 'sharp';
import assert from 'node:assert/strict';
import { findPort } from './platform.mjs';
const mode=process.argv[2]||'status';
const port=new SerialPort({path:await findPort(),baudRate:115200,autoOpen:false});
const pending=new Map();let seq=0,buffer='',installing=false;
const decoder=new StringDecoder('utf8');
port.on('data',data=>{
  buffer+=decoder.write(data);
  while(buffer.includes('\n')) {
    const end=buffer.indexOf('\n'),line=buffer.slice(0,end).trim();buffer=buffer.slice(end+1);
    if(!line.startsWith('@')) {if(line)console.error(line);continue;}
    let response;
    try {response=JSON.parse(line.slice(1));} catch(e) {console.error('Malformed device response:',e.message);continue;}
    const entry=pending.get(response.id);
    if(entry){clearTimeout(entry.timer);pending.delete(response.id);response.ok?entry.resolve(response):entry.reject(Error(response.error));}
    else console.error('Unmatched device response',response);
  }
});
port.on('error',error=>{for(const p of pending.values()){clearTimeout(p.timer);p.reject(error);}pending.clear();});
function request(cmd,fields={},timeout=15000) {
  return new Promise((resolve,reject)=>{
    const id=++seq,timer=setTimeout(()=>{pending.delete(id);reject(Error(`Timeout: ${cmd}`));},timeout);
    pending.set(id,{resolve,reject,timer});
    port.write(JSON.stringify({id,cmd,...fields})+'\n',error=>{
      if(error){clearTimeout(timer);pending.delete(id);reject(error);}
    });
  });
}
try {
  port.open();await once(port,'open');
  await new Promise(resolve=>setTimeout(resolve,300));
  if(mode==='status') console.log(JSON.stringify(await request('status'),null,2));
  else if(mode==='time') console.log(await request('time',{utc:Math.floor(Date.now()/1000)}));
  else if(mode==='verify-battery') {
    const wait=ms=>new Promise(resolve=>setTimeout(resolve,ms));
    const state=()=>request('status');
    const initial=await state();
    const restore={0:1,1:2,3:4,4:5,5:6,6:7,7:9}[initial.page];
    assert(restore&&!initial.bubble_game&&initial.page!==8&&!initial.asset_loading,
      'Finish the current game, time edit or asset load before verifying');
    const checkState=s=>{
      assert(['ready','disconnected','unavailable'].includes(s.battery_state),'Battery diagnostics unavailable');
      if(s.battery_state==='ready') {
        assert(Number.isInteger(s.battery_percent)&&s.battery_percent>=0&&s.battery_percent<=100);
        assert.equal(s.battery_text,`${s.battery_percent}%`);
      } else {
        assert.equal(s.battery_percent,null);
        assert.equal(s.battery_text,s.battery_state==='disconnected'?'--':'ERR');
      }
      assert.equal(s.battery_visible,true);
      assert.equal(s.battery_clickable,false);
      assert(s.battery_text_width_px<=s.battery_content_width_px,'Current battery text does not fit');
      assert(s.battery_max_width_px<=s.battery_content_width_px,'100% battery text does not fit');
    };
    const captureIndicator=async()=>{
      await request('capture_begin');
      try {
        const parts=[];
        for(let row=16;row<60;row+=4) {  // battery pill: x 164..316, y 16..60
          const pixels=Buffer.from((await request('capture_row',{row})).hex,'hex');
          assert.equal(pixels.length,480*4*2);
          for(let y=0;y<4;y++)parts.push(pixels.subarray((y*480+164)*2,(y*480+316)*2));
        }
        return Buffer.concat(parts);
      } finally {await request('capture_end');}
    };
    const pages=[[1,0,'game'],[2,1,'ambient'],[4,3,'settings'],
      [5,4,'foods'],[6,5,'toys'],[7,6,'rooms'],[9,7,'status'],[59,8,'set-time']];
    const skipped=[];
    if(!initial.sleeping&&initial.energy>=800&&!initial.save_locked)pages.push([31,5,'bubble']);
    else skipped.push('bubble: pet must be awake with enough energy and an unlocked save');
    let reference=null,referenceText='',last;
    try {
      await request('screen',{on:true});
      for(const [id,page,name] of pages) {
        await request('control',{value:id});await wait(300);
        last=await state();checkState(last);
        assert.equal(last.page,page,`${name} page did not open`);
        if(name==='bubble')assert(last.bubble_game,'Bubble game did not open');
        const image=await captureIndicator();
        const colors=new Set();
        for(let i=0;i<image.length;i+=2)colors.add(image.readUInt16LE(i));
        assert(colors.size>1,`Battery text not rendered on ${name}`);
        if(reference) {
          assert.equal(last.battery_text,referenceText,'Battery changed during verification; retry');
          assert(image.equals(reference),`Battery indicator differs on ${name}`);
        } else {reference=image;referenceText=last.battery_text;}
      }
      const firstSample=last.battery_sample_ms;
      await request('screen',{on:false});
      assert.equal((await state()).display_on,false);
      await wait(1200);
      last=await state();checkState(last);
      assert.notEqual(last.battery_sample_ms,firstSample,'Battery reading did not refresh with the screen off');
      await request('screen',{on:true});await wait(300);
      last=await state();checkState(last);
      assert.equal(last.display_on,true);
      assert.equal(last.battery_text,referenceText,'Battery changed during verification; retry');
      assert((await captureIndicator()).equals(reference),'Battery indicator differs after waking the screen');
      console.log(JSON.stringify({verified:true,battery_state:last.battery_state,battery_percent:last.battery_percent,
        battery_text:last.battery_text,tested_pages:pages.map(p=>p[2]),screenOffAndOn:true,skipped,
        text_width_px:last.battery_text_width_px,max_text_width_px:last.battery_max_width_px,
        content_width_px:last.battery_content_width_px},null,2));
    } finally {
      await request('control',{value:restore});
      await request('screen',{on:initial.display_on});
    }
  }
  else if(mode==='verify') {
    const wait=ms=>new Promise(resolve=>setTimeout(resolve,ms));
    const state=()=>request('status');
    const control=async value=>{await request('control',{value});await wait(1000);return state();};
    const initial=await state();
    assert(initial.sd&&initial.touch&&initial.audio&&initial.pmic&&initial.rtc_valid);
    assert(initial.charge_configured&&!initial.save_locked&&initial.pack);
    await request('screen',{on:true});
    for(const [id,page] of [[1,0],[2,1],[4,3],[9,7]])assert.equal((await control(id)).page,page);
    await control(1);
    for(const [id,page] of [[5,4],[6,5],[7,6]]) {
      assert.equal((await control(id)).page,page);
      assert.equal((await control(10)).page,0);
    }
    if((await state()).sleeping)await control(8);
    await control(8);
    const sleeping=await state();assert(sleeping.sleeping);
    await control(20);
    assert.equal((await state()).bond,sleeping.bond,'Feeding during sleep must not earn affection');
    await control(8);
    const hungry=await state();
    const fed=await control(20);
    assert.equal(fed.bond,hungry.bond+(hungry.hunger<9500?5:0));
    await control(1);
    const beforePlay=await state();
    assert(beforePlay.energy>=800,'Charge pet energy through sleep before running verify');
    assert.equal((await control(30)).bond,beforePlay.bond+8);
    await control(1);
    assert((await control(31)).bubble_game);
    for(let i=0;i<5;i++)await control(67);
    const played=await state();assert(!played.bubble_game);assert.equal(played.bond,beforePlay.bond+16);
    const rooms=[...Array(initial.background_count).keys()].filter(i=>i!==initial.background);
    for(const room of [...rooms,initial.background]) {
      await control(1);await control(40+room);
      let loaded;
      for(let i=0;i<30;i++){loaded=await state();if(!loaded.asset_loading)break;await wait(500);}
      assert(!loaded.asset_loading&&loaded.pack);assert.equal(loaded.background,room);
    }
    await assert.rejects(request('control',{value:19}),/invalid control/);
    await assert.rejects(request('pack_begin',{name:'../invalid'}),/invalid pack name/);
    await assert.rejects(request('time',{utc:1}),/invalid time/);
    await request('screen',{on:false});assert.equal((await state()).display_on,false);
    await request('screen',{on:true});assert.equal((await state()).display_on,true);
    await control(1);
    if(initial.sleeping)await control(8);
    await request('save');
    const saved=await state();
    assert.equal(saved.audio_failures,0);
    assert(saved.heap>30000&&saved.psram_free>2*1024*1024);
    await request('reboot');await wait(7000);
    const rebooted=await state();
    assert.equal(rebooted.bond,saved.bond);assert.equal(rebooted.background,saved.background);
    assert.equal(rebooted.sleeping,saved.sleeping);assert.equal(rebooted.pack,saved.pack);
    assert(rebooted.rtc_valid&&Math.abs(rebooted.rtc_utc-rebooted.utc)<=1);
    assert(rebooted.save_generation>saved.save_generation);
    await request('screen',{on:true});await control(1);
    console.log(JSON.stringify({verified:true,bond:rebooted.bond,save_generation:rebooted.save_generation,
      heap:rebooted.heap,psram_free:rebooted.psram_free,pack:rebooted.pack,
      physicalTouchAndAudibility:'not verified by this software check'},null,2));
  }
  else if(mode==='install') {
    const dir=process.argv[3]||'assets/pack';
    const manifest=readFileSync(`${dir}/manifest.json`),m=JSON.parse(manifest);
    const files=[...Object.keys(m.files),'manifest.json'];
    const name=`pack-${Date.now().toString(36)}`;
    console.log(await request('pack_begin',{name}));
    installing=true;
    for(const name of files) {
      const data=readFileSync(`${dir}/${name}`),sha256=createHash('sha256').update(data).digest('hex');
      if(name!=='manifest.json'&&(sha256!==m.files[name].sha256||data.length!==m.files[name].bytes)) throw Error(`Local pack hash invalid: ${name}`);
      await request('file_begin',{name,size:data.length,sha256});
      for(let offset=0;offset<data.length;offset+=1024) await request('chunk',{offset,hex:data.subarray(offset,offset+1024).toString('hex')});
      await request('file_end');console.log(`Verified ${name}: ${data.length} bytes`);
    }
    console.log(await request('pack_end',{},120000));
    installing=false;
  } else if(mode==='capture') {
    const file=process.argv[3]||'build/device.png';
    await request('capture_begin');const chunks=[];
    try {for(let row=0;row<480;row+=4)chunks.push(Buffer.from((await request('capture_row',{row})).hex,'hex'));}
    finally {await request('capture_end');}
    const raw=Buffer.concat(chunks),rgb=Buffer.alloc(480*480*3);
    for(let i=0;i<480*480;i++){const c=raw.readUInt16LE(i*2);rgb[i*3]=Math.round((c>>11)*255/31);rgb[i*3+1]=Math.round(((c>>5)&63)*255/63);rgb[i*3+2]=Math.round((c&31)*255/31);}
    await sharp(rgb,{raw:{width:480,height:480,channels:3}}).png().toFile(file);console.log(file);
  } else if(mode==='command') console.log(JSON.stringify(await request(process.argv[3],JSON.parse(process.argv[4]||'{}'),120000),null,2));
  else if(mode==='monitor') await new Promise(resolve=>setTimeout(resolve,Number(process.argv[3]||30)*1000));
  else throw Error('Usage: device.mjs status|time|verify|verify-battery|install [pack]|capture [png]|command NAME [JSON]|monitor [seconds]');
} catch(error) {
  console.error(error.message);process.exitCode=1;
  if(installing&&port.isOpen) {
    try {await request('abort');}
    catch(abortError){console.error(`Transfer cleanup failed: ${abortError.message}`);}
  }
} finally {
  for(const entry of pending.values())clearTimeout(entry.timer);
  if(port.isOpen) await new Promise((resolve,reject)=>port.close(err=>err?reject(err):resolve()));
}
