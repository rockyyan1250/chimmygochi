#include "Kmb.h"
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include "KmbCert.h"

namespace kmb {
namespace {
SemaphoreHandle_t lock=nullptr;
Result result;
volatile bool running=false;
Request jobs[kMaxBoards];
unsigned jobCount=0;

// "2026-11-02T13:05:00+08:00" -> UTC epoch seconds; 0 if malformed.
int64_t parseIso(const char *s) {
  int y,mo,d,h,mi,sec,oh=0,om=0;char sign='+';
  if(!s||sscanf(s,"%d-%d-%dT%d:%d:%d%c%d:%d",&y,&mo,&d,&h,&mi,&sec,&sign,&oh,&om)<6) return 0;
  // Days from 1970-01-01 (proleptic Gregorian), Howard Hinnant's algorithm.
  y-=mo<=2;const int era=(y>=0?y:y-399)/400;const unsigned yoe=unsigned(y-era*400);
  const unsigned doy=(153*(mo+(mo>2?-3:9))+2)/5+d-1,doe=yoe*365+yoe/4-yoe/100+doy;
  const int64_t days=int64_t(era)*146097+int64_t(doe)-719468;
  const int64_t offset=(oh*3600+om*60)*(sign=='-'?-1:1);
  return days*86400+h*3600+mi*60+sec-offset;
}

void fetch(NetworkClientSecure &client,const Request &q,Board &b) {
  HTTPClient http;http.setTimeout(8000);
  char url[160];
  snprintf(url,sizeof(url),"https://data.etabus.gov.hk/v1/transport/kmb/eta/%s/%s/%u",q.stop,q.route,q.serviceType);
  if(!http.begin(client,url)) {snprintf(b.error,sizeof(b.error),"http begin");return;}
  b.http=http.GET();
  if(b.http!=200) {snprintf(b.error,sizeof(b.error),"HTTP %d",b.http);http.end();return;}
  JsonDocument filter;
  for(const char *k:{"dir","dest_tc","rmk_tc","eta"}) filter["data"][0][k]=true;
  JsonDocument doc;
  const auto err=deserializeJson(doc,http.getStream(),DeserializationOption::Filter(filter));
  http.end();
  if(err) {snprintf(b.error,sizeof(b.error),"JSON %s",err.c_str());return;}
  // A stop served in both directions lists both; keep the chosen one.
  for(JsonObject a:doc["data"].as<JsonArray>()) {
    const char *dir=a["dir"]|"";
    if(q.bound&&dir[0]!=q.bound) continue;
    if(b.count>=kMaxArrivals) break;
    auto &x=b.arrivals[b.count++];
    snprintf(x.dest,sizeof(x.dest),"%s",a["dest_tc"]|"");
    snprintf(x.remark,sizeof(x.remark),"%s",a["rmk_tc"]|"");
    x.eta=parseIso(a["eta"]|"");
  }
  b.ok=true;
}

void task(void *) {
  // Kept off the task stack and filled in place; published as a whole under the lock.
  static Result r;r=Result{};
  r.heapBefore=heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  const uint32_t t0=millis();
  {
    NetworkClientSecure client;client.setCACert(kKmbRootCa);
    for(unsigned i=0;i<jobCount;i++) fetch(client,jobs[i],r.boards[i]);
  }
  r.boardCount=jobCount;r.requestMs=millis()-t0;
  r.heapLowAfter=heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
  r.stackFree=uxTaskGetStackHighWaterMark(nullptr);
  r.done=true;r.finishedMs=millis();
  xSemaphoreTake(lock,portMAX_DELAY);result=r;running=false;xSemaphoreGive(lock);
  vTaskDelete(nullptr);
}
bool ok(const Request &q) {
  if(!q.route[0]||!q.stop[0]||!q.serviceType) return false;
  for(const char *p=q.route;*p;p++) if(!isalnum(uint8_t(*p))) return false;
  for(const char *p=q.stop;*p;p++) if(!isalnum(uint8_t(*p))) return false;
  return true;
}
}  // namespace

bool start(const Request *requests,unsigned count) {
  if(!lock) lock=xSemaphoreCreateMutex();
  if(!lock||running||!count||count>kMaxBoards||WiFi.status()!=WL_CONNECTED) return false;
  for(unsigned i=0;i<count;i++) if(!ok(requests[i])) return false;
  memcpy(jobs,requests,sizeof(Request)*count);jobCount=count;
  running=true;
  // TLS handshakes need a deep stack; core 0 keeps the UI loop (core 1) responsive.
  if(xTaskCreatePinnedToCore(task,"kmb",16384,nullptr,1,nullptr,0)!=pdPASS) {running=false;return false;}
  return true;
}
bool busy() {return running;}
Result last() {
  if(!lock) return Result{};
  xSemaphoreTake(lock,portMAX_DELAY);Result r=result;xSemaphoreGive(lock);return r;
}
}  // namespace kmb
