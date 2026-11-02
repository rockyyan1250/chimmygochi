#include "Bus.h"
#include <ArduinoJson.h>
#include <Preferences.h>
#include <WebServer.h>
#include <esp_random.h>

namespace bus {
namespace {
Line lines[kLines];
char pinCode[5];
uint32_t rev=1;

bool valid(const Line &l) {
  if(!l.route[0]) return true;  // empty slot
  if(l.bound!='O'&&l.bound!='I'||!l.serviceType||!l.stop[0]) return false;
  for(const char *p=l.route;*p;p++) if(!isalnum(uint8_t(*p))) return false;
  for(const char *p=l.stop;*p;p++) if(!isalnum(uint8_t(*p))) return false;
  return true;
}
void save() {
  Preferences p;if(!p.begin("cgbus",false)) {Serial.println("ERROR bus config save");return;}
  p.putBytes("lines",lines,sizeof(lines));p.end();rev++;
}

const char kPage[] PROGMEM = R"HTML(<!doctype html><html lang="zh-HK"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>CHIMMY 巴士設定</title>
<style>body{font-family:sans-serif;background:#fff8e1;color:#4a3f4f;margin:0;padding:16px;max-width:460px}
h1{font-size:22px}.card{background:#fff;border-radius:16px;padding:12px;margin:12px 0}
button,input,select{font-size:17px;width:100%;box-sizing:border-box;padding:10px;margin:5px 0;border-radius:10px;border:1px solid #d8c9a8}
button{background:#ffd23f;border:0;font-weight:bold}.cur{color:#7a6e7f;font-size:15px}#msg{font-size:17px;margin:10px 0}</style></head><body>
<h1>🚌 CHIMMY 巴士設定</h1>
<p>揀兩條九巴路線同車站，CHIMMY 會幫你睇住下一班車幾時到。</p>
<div id="slots"></div>
<button onclick="save()">儲存</button>
<div id="msg">載入緊路線資料…</div>
<script>
const $=id=>document.getElementById(id),pin=new URLSearchParams(location.search).get('pin')||'';
const K='https://data.etabus.gov.hk/v1/transport/kmb/';
let routes=[],names={},cur=[],pick=[{},{}];
const esc=s=>String(s).replace(/[&<>"]/g,c=>'&#'+c.charCodeAt(0)+';');
async function boot(){
 cur=(await (await fetch('/api/bus')).json()).lines;
 [routes,names]=await Promise.all([fetch(K+'route/').then(r=>r.json()).then(j=>j.data),
  fetch(K+'stop').then(r=>r.json()).then(j=>Object.fromEntries(j.data.map(s=>[s.stop,s.name_tc])))]);
 $('slots').innerHTML=[0,1].map(i=>`<div class="card"><b>路線 ${i+1}</b>
  <div class="cur">而家：${cur[i].route?esc(cur[i].route+' 往 '+cur[i].dest+'｜'+cur[i].stopName):'未設定'}</div>
  <input id="r${i}" placeholder="路線號碼，例如 1A" oninput="dirs(${i})">
  <select id="d${i}" onchange="stops(${i})"></select><select id="s${i}" onchange="choose(${i})"></select>
  <button onclick="clearSlot(${i})" style="background:#eee">清除呢條路線</button></div>`).join('');
 $('msg').textContent='';}
function dirs(i){const r=$('r'+i).value.trim().toUpperCase(),opts=routes.filter(x=>x.route==r);
 $('d'+i).innerHTML='<option value="">揀方向</option>'+opts.map((x,k)=>`<option value="${k}">往 ${esc(x.dest_tc)}（由 ${esc(x.orig_tc)}）${x.service_type!='1'?' 特別班':''}</option>`).join('');
 $('d'+i).opts=opts;$('s'+i).innerHTML='';pick[i]={};}
async function stops(i){const x=$('d'+i).opts[$('d'+i).value];if(!x)return;
 const dir=x.bound=='O'?'outbound':'inbound';
 const list=(await (await fetch(K+`route-stop/${x.route}/${dir}/${x.service_type}`)).json()).data;
 $('s'+i).innerHTML='<option value="">揀車站</option>'+list.map(s=>`<option value="${s.stop}">${s.seq}. ${esc(names[s.stop]||s.stop)}</option>`).join('');
 pick[i]={route:x.route,bound:x.bound,st:+x.service_type,dest:x.dest_tc};}
function choose(i){const id=$('s'+i).value;if(id){pick[i].stop=id;pick[i].stopName=names[id]||id;}}
function clearSlot(i){pick[i]={clear:true};$('msg').textContent='撳「儲存」就會清除路線 '+(i+1);}
async function save(){
 const lines=[0,1].map(i=>pick[i].clear?{}:pick[i].stop?pick[i]:cur[i]);
 const r=await fetch('/api/bus',{method:'POST',body:JSON.stringify({pin,lines})});
 $('msg').textContent=r.ok?'儲存咗！睇下 CHIMMY 嘅巴士畫面 🐶':r.status==403?'PIN 唔啱，請喺 CHIMMY 再掃一次 QR Code':'儲存失敗，請再試';
 if(r.ok)cur=lines;}
boot().catch(()=>$('msg').textContent='載入失敗：請確認手機可以上網');
</script></body></html>)HTML";
}  // namespace

void begin() {
  Preferences p;
  if(p.begin("cgbus",true)) {
    if(p.getBytesLength("lines")==sizeof(lines)) p.getBytes("lines",lines,sizeof(lines));
    p.end();
  }
  for(auto &l:lines) {l.route[sizeof(l.route)-1]=l.stop[sizeof(l.stop)-1]=0;
    l.stopName[sizeof(l.stopName)-1]=l.dest[sizeof(l.dest)-1]=0;if(!valid(l)) l=Line{};}
  snprintf(pinCode,sizeof(pinCode),"%04u",unsigned(esp_random()%10000));
}
void attach(WebServer &web) {
  web.on("/bus",HTTP_GET,[&web]{web.send_P(200,"text/html; charset=utf-8",kPage);});
  web.on("/api/bus",HTTP_GET,[&web]{
    JsonDocument doc;JsonArray list=doc["lines"].to<JsonArray>();
    for(const auto &l:lines) {
      JsonObject o=list.add<JsonObject>();
      if(!l.route[0]) continue;
      o["route"]=(char *)l.route;o["bound"]=String(l.bound);o["st"]=l.serviceType;
      o["stop"]=(char *)l.stop;o["stopName"]=(char *)l.stopName;o["dest"]=(char *)l.dest;
    }
    String out;serializeJson(doc,out);web.send(200,"application/json",out);
  });
  web.on("/api/bus",HTTP_POST,[&web]{
    JsonDocument doc;
    if(deserializeJson(doc,web.arg("plain"))||doc["lines"].size()!=kLines) {web.send(400,"text/plain","bad request");return;}
    if(strcmp(doc["pin"]|"",pinCode)) {web.send(403,"text/plain","bad pin");return;}
    Line next[kLines]{};
    for(unsigned i=0;i<kLines;i++) {
      JsonObject o=doc["lines"][i];auto &l=next[i];
      snprintf(l.route,sizeof(l.route),"%s",o["route"]|"");
      if(!l.route[0]) continue;
      const char *b=o["bound"]|"";l.bound=b[0];l.serviceType=o["st"]|0;
      snprintf(l.stop,sizeof(l.stop),"%s",o["stop"]|"");
      snprintf(l.stopName,sizeof(l.stopName),"%s",o["stopName"]|"");
      snprintf(l.dest,sizeof(l.dest),"%s",o["dest"]|"");
      if(!valid(l)) {web.send(400,"text/plain","invalid line");return;}
    }
    memcpy(lines,next,sizeof(lines));save();
    web.send(200,"application/json","{\"ok\":true}");
  });
}
const Line &line(unsigned i) {return lines[i<kLines?i:0];}
bool used(unsigned i) {return i<kLines&&lines[i].route[0];}
const char *pin() {return pinCode;}
uint32_t revision() {return rev;}
}  // namespace bus
