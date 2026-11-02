#include "Net.h"
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_random.h>

namespace net {
namespace {
struct Saved {char ssid[33];char pass[65];};
Saved saved[kMaxSaved];
unsigned count=0,attempt=0;
State current=State::Off;
uint32_t attemptAt=0,retryAt=0,joinAt=0,joinedAt=0;
bool fresh=false;

bool portal=false,serving=false;
Join join=Join::Idle;
Saved pending;
char apName[20],apPass[12],qr[64];
DNSServer dns;
WebServer server(80);
// The HTTP server needs the network stack, which exists only once Wi-Fi has been started;
// starting it earlier asserted inside lwIP at boot. Start on the first connection or the portal.
void serve() {if(!serving) {server.begin();serving=true;}}

constexpr uint32_t kAttemptMs=12000,kJoinMs=15000,kRetryMs=60000,kPortalAfterJoinMs=30000;

void load() {
  Preferences p;if(!p.begin("cgwifi",true)) return;
  count=std::min<unsigned>(p.getUChar("n",0),kMaxSaved);
  for(unsigned i=0;i<count;i++) {
    char k[4];snprintf(k,sizeof(k),"s%u",i);p.getString(k,saved[i].ssid,sizeof(saved[i].ssid));
    snprintf(k,sizeof(k),"p%u",i);p.getString(k,saved[i].pass,sizeof(saved[i].pass));
  }
  p.end();
}
// Newest first; an existing entry with the same SSID is replaced. NVS is not encrypted.
void remember(const Saved &s) {
  Saved list[kMaxSaved];unsigned n=0;list[n++]=s;
  for(unsigned i=0;i<count&&n<kMaxSaved;i++) if(strcmp(saved[i].ssid,s.ssid)) list[n++]=saved[i];
  memcpy(saved,list,sizeof(list));count=n;
  Preferences p;if(!p.begin("cgwifi",false)) {Serial.println("ERROR Wi-Fi list save");return;}
  p.putUChar("n",count);
  for(unsigned i=0;i<count;i++) {
    char k[4];snprintf(k,sizeof(k),"s%u",i);p.putString(k,saved[i].ssid);
    snprintf(k,sizeof(k),"p%u",i);p.putString(k,saved[i].pass);
  }
  p.end();
}
void tryNetwork(unsigned i) {
  attempt=i;attemptAt=millis();current=State::Connecting;
  WiFi.begin(saved[i].ssid,saved[i].pass);
}

const char kPage[] PROGMEM = R"HTML(<!doctype html><html lang="zh-HK"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>CHIMMY Wi-Fi 設定</title>
<style>body{font-family:sans-serif;background:#fff8e1;color:#4a3f4f;margin:0;padding:20px;max-width:420px}
h1{font-size:22px}button,input,select{font-size:18px;width:100%;box-sizing:border-box;padding:12px;margin:6px 0;border-radius:12px;border:1px solid #d8c9a8}
button{background:#ffd23f;border:0;font-weight:bold}#msg{margin-top:12px;font-size:17px}</style></head><body>
<h1>🐶 CHIMMY Wi-Fi 設定</h1>
<p>揀你屋企嘅 Wi-Fi，輸入密碼之後撳「連接」。</p>
<select id="ssid"><option>搵緊 Wi-Fi…</option></select>
<input id="other" placeholder="或者自己輸入 Wi-Fi 名稱">
<input id="pass" type="password" placeholder="Wi-Fi 密碼">
<button onclick="join()">連接</button>
<div id="msg"></div>
<script>
const $=id=>document.getElementById(id);
async function scan(){try{const r=await(await fetch('/scan')).json();
 if(!r.length){setTimeout(scan,2000);return;}
 $('ssid').innerHTML=r.map(n=>`<option>${n.s.replace(/</g,'&lt;')}</option>`).join('');}catch(e){setTimeout(scan,2000);}}
async function join(){const s=$('other').value||$('ssid').value;
 $('msg').textContent='連接緊「'+s+'」…';
 await fetch('/join',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},
  body:'ssid='+encodeURIComponent(s)+'&pass='+encodeURIComponent($('pass').value)});
 poll();}
async function poll(){try{const r=await(await fetch('/status')).json();
 if(r.join=='joined'){$('msg').textContent='連接咗！CHIMMY 已經記住呢個 Wi-Fi 🎉';return;}
 if(r.join=='rejected'){$('msg').textContent='連唔到，請檢查密碼再試多次。';return;}}
 catch(e){$('msg').textContent='手機可能斷咗線，請睇返 CHIMMY 嘅畫面。';return;}
 setTimeout(poll,1500);}
scan();
</script></body></html>)HTML";

void routes() {
  // One server for both: the Wi-Fi page while the setup AP is up, otherwise the bus page.
  server.on("/",HTTP_GET,[]{
    if(portal) server.send_P(200,"text/html; charset=utf-8",kPage);
    else {server.sendHeader("Location","/bus");server.send(302,"text/plain","");}
  });
  server.on("/scan",HTTP_GET,[]{
    const int n=WiFi.scanComplete();
    if(n==WIFI_SCAN_FAILED) WiFi.scanNetworks(true);
    String json="[";
    for(int i=0;i<n;i++) {
      String s=WiFi.SSID(i);if(!s.length()) continue;
      s.replace("\\","\\\\");s.replace("\"","\\\"");
      if(json.length()>1) json+=",";
      json+="{\"s\":\""+s+"\",\"r\":"+String(WiFi.RSSI(i))+"}";
    }
    server.send(200,"application/json",json+"]");
  });
  server.on("/join",HTTP_POST,[]{
    const String s=server.arg("ssid"),p=server.arg("pass");
    if(!s.length()||s.length()>32||p.length()>64) {server.send(400,"application/json","{\"ok\":false}");return;}
    addNetwork(s.c_str(),p.c_str());
    server.send(200,"application/json","{\"ok\":true}");
  });
  server.on("/status",HTTP_GET,[]{
    const char *names[]={"idle","trying","joined","rejected"};
    server.send(200,"application/json",String("{\"join\":\"")+names[int(join)]+"\"}");
  });
  // Captive portal: every other URL (OS connectivity checks included) lands on the page.
  server.onNotFound([]{
    if(!portal) {server.send(404,"text/plain","not found");return;}
    server.sendHeader("Location","http://192.168.4.1/");server.send(302,"text/plain","");
  });
}
}  // namespace

void begin() {
  WiFi.persistent(false);WiFi.setAutoReconnect(false);
  load();routes();
  if(count) {WiFi.mode(WIFI_STA);tryNetwork(0);}
}
void tick() {
  const uint32_t now=millis();
  const bool up=WiFi.status()==WL_CONNECTED;
  if(join==Join::Trying) {
    if(up) {remember(pending);join=Join::Joined;joinedAt=now;current=State::Connected;fresh=true;serve();MDNS.end();MDNS.begin("chimmy");}
    else if(now-joinAt>kJoinMs) {join=Join::Rejected;WiFi.disconnect(false,false);current=State::Failed;retryAt=now+5000;}
  } else if(up) {
    if(current!=State::Connected) {current=State::Connected;fresh=true;serve();MDNS.end();MDNS.begin("chimmy");}
  } else if(current==State::Connected) {
    if(count) tryNetwork(0); else current=State::Off;  // dropped: start over from the newest
  } else if(current==State::Connecting&&now-attemptAt>kAttemptMs) {
    if(attempt+1<count) tryNetwork(attempt+1);
    else {current=State::Failed;retryAt=now+kRetryMs;WiFi.disconnect(false,false);}
  } else if(current==State::Failed&&count&&int32_t(now-retryAt)>=0) tryNetwork(0);
  if(serving) server.handleClient();
  if(portal) {
    dns.processNextRequest();
    if(join==Join::Joined&&now-joinedAt>kPortalAfterJoinMs) stopPortal();
  }
}
State state() {return current;}
String ssid() {return current==State::Connected?WiFi.SSID():String();}
int rssi() {return current==State::Connected?WiFi.RSSI():0;}
unsigned savedCount() {return count;}
bool startPortal() {
  if(portal) return true;
  uint8_t mac[6];WiFi.macAddress(mac);
  snprintf(apName,sizeof(apName),"CHIMMY-%02X%02X",mac[4],mac[5]);
  const char alphabet[]="abcdefghjkmnpqrstuvwxyz23456789";  // no look-alike characters
  for(unsigned i=0;i<8;i++) apPass[i]=alphabet[esp_random()%(sizeof(alphabet)-1)];
  apPass[8]=0;
  snprintf(qr,sizeof(qr),"WIFI:T:WPA;S:%s;P:%s;;",apName,apPass);
  WiFi.mode(WIFI_AP_STA);serve();
  if(!WiFi.softAP(apName,apPass)) {Serial.println("ERROR soft AP");WiFi.mode(count?WIFI_STA:WIFI_OFF);return false;}
  dns.start(53,"*",WiFi.softAPIP());
  WiFi.scanNetworks(true);
  portal=true;join=Join::Idle;
  return true;
}
void stopPortal() {
  if(!portal) return;
  dns.stop();WiFi.softAPdisconnect(true);WiFi.scanDelete();
  WiFi.mode(count?WIFI_STA:WIFI_OFF);
  portal=false;if(join!=Join::Trying) join=Join::Idle;
}
bool portalActive() {return portal;}
Join joinState() {return join;}
const char *portalName() {return apName;}
const char *portalQr() {return qr;}
bool addNetwork(const char *s,const char *p) {
  if(!s||!*s||strlen(s)>32||!p||strlen(p)>64) return false;
  snprintf(pending.ssid,sizeof(pending.ssid),"%s",s);snprintf(pending.pass,sizeof(pending.pass),"%s",p);
  if(!portal) WiFi.mode(WIFI_STA);
  WiFi.disconnect(false,false);WiFi.begin(pending.ssid,pending.pass);
  join=Join::Trying;joinAt=millis();current=State::Connecting;
  return true;
}
WebServer &web() {return server;}
String ip() {return current==State::Connected?WiFi.localIP().toString():String();}
bool connectedOnce() {const bool f=fresh;fresh=false;return f;}
}  // namespace net
