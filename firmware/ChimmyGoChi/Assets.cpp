#include "Assets.h"
#include <Preferences.h>
#include <cstdio>

Assets assets;
static constexpr const char *sounds[]={"tap","happy","eat","dress"};
class SdGuard {
 public:
  SdGuard(){xSemaphoreTake(assets.mutex,portMAX_DELAY);}
  ~SdGuard(){xSemaphoreGive(assets.mutex);}
};
bool Assets::begin() {
  mutex=xSemaphoreCreateMutex();
  if(!mutex) {Serial.println("ERROR SD mutex");return false;}
  spi.begin(2,3,1,41);
  ready=SD.begin(41,spi,20000000);
  if(!ready) {Serial.println("ERROR SD mount; card NOT formatted");return false;}
  requests=xQueueCreate(1,sizeof(Request));results=xQueueCreate(1,sizeof(AssetResult));
  if(!requests||!results||xTaskCreate(worker,"asset-io",8192,this,1,nullptr)!=pdPASS) {
    Serial.println("ERROR asset worker");ready=false;
  }
  return ready;
}
String Assets::activeRoot() {
  Preferences p;
  if(!p.begin("cgcontent",false)) {Serial.println("ERROR content preferences");return "";}
  String value=p.getString("active","");p.end();
  if(!value.startsWith("/chimmygochi/packs/")||
     !game::safeName(value.substring(strlen("/chimmygochi/packs/")).c_str())) return "";
  return value;
}
bool Assets::activate(const char *root) {
  Preferences p;
  if(!p.begin("cgcontent",false)) return false;
  const bool ok=p.putString("active",root)==strlen(root);
  p.end();return ok;
}
bool Assets::request(const char *root,uint8_t background,uint32_t generation,const AssetPack *reuse) {
  if(!ready||!root||!*root||strlen(root)>=96||background>=kMaxBackgrounds) return false;
  Request r{};snprintf(r.root,sizeof(r.root),"%s",root);r.background=background;r.generation=generation;r.reuse=reuse;
  return xQueueSend(requests,&r,0)==pdTRUE;
}
bool Assets::poll(AssetResult &r) {return ready&&xQueueReceive(results,&r,0)==pdTRUE;}
void Assets::worker(void *arg) {
  auto self=static_cast<Assets *>(arg);Request r;
  for(;;) if(xQueueReceive(self->requests,&r,portMAX_DELAY)==pdTRUE) {
    AssetResult result{};result.generation=r.generation;
    self->load(r,result);
    xQueueSend(self->results,&result,portMAX_DELAY);
  }
}
static bool hashData(const uint8_t *data,size_t size,const char *expected) {
  if(!expected||strlen(expected)!=64) return false;
  uint8_t digest[32];char hex[65]{};
  if(mbedtls_sha256(data,size,digest,0)!=0) return false;
  for(unsigned i=0;i<32;i++) snprintf(hex+2*i,3,"%02x",digest[i]);
  return strcmp(hex,expected)==0;
}
bool Assets::readBlob(const String &path,Blob &blob,size_t max,const char *hash) {
  File f;
  {SdGuard lock;f=SD.open(path);if(!f||f.isDirectory()) return false;
    blob.size=f.size();}
  if(!blob.size||blob.size>max) {SdGuard lock;f.close();return false;}
  blob.data=static_cast<uint8_t *>(heap_caps_malloc(blob.size,MALLOC_CAP_SPIRAM));
  if(!blob.data) {SdGuard lock;f.close();return false;}
  size_t offset=0;
  while(offset<blob.size) {
    size_t n;
    {SdGuard lock;n=f.read(blob.data+offset,std::min<size_t>(16384,blob.size-offset));}
    if(n==0) break;
    offset+=n;vTaskDelay(1);
  }
  {SdGuard lock;f.close();}
  return offset==blob.size && (!hash||hashData(blob.data,blob.size,hash));
}
bool Assets::load(const Request &r,AssetResult &result) {
  auto fail=[&](const char *why){snprintf(result.error,sizeof(result.error),"%s",why);Serial.printf("ERROR assets %s\n",why);return false;};
  auto pack=std::unique_ptr<AssetPack>(new AssetPack);
  Blob manifest;
  if(!readBlob(String(r.root)+"/manifest.json",manifest,65536,nullptr)) return fail("manifest read failed");
  JsonDocument doc;
  // Schema 2: one atlas + 1..3 backgrounds with thumbnails. Schema 1 (three outfits, one
  // background) still loads, using the first outfit.
  if(deserializeJson(doc,manifest.data,manifest.size)) return fail("manifest JSON invalid");
  const int schema=doc["schema"]|0;
  const char *atlasName=schema==2?doc["atlas"].as<const char *>():
    schema==1?doc["outfits"][0]["atlas"].as<const char *>():nullptr;
  const size_t backgrounds=schema==2?doc["backgrounds"].size():1;
  if(!atlasName||!backgrounds||backgrounds>kMaxBackgrounds) return fail("manifest schema invalid");
  pack->placeholder=doc["placeholder"]|true;
  pack->backgroundCount=backgrounds;pack->selected=r.background<backgrounds?r.background:0;
  snprintf(pack->root,sizeof(pack->root),"%s",r.root);
  auto image=[&](const char *name,Blob &blob,game::ImageHeader &header,uint8_t bpp) {
    if(!game::safeName(name)) return false;
    const char *hash=doc["files"][name]["sha256"];
    if(!hash||!readBlob(String(r.root)+"/"+name,blob,4*1024*1024+16,hash)||
       doc["files"][name]["bytes"].as<size_t>()!=blob.size) return false;
    return game::imageHeader(blob.data,blob.size,header)&&header.bpp==bpp;
  };
  // Same pack already on screen (background change): share its verified atlas, so memory never
  // holds two copies of the largest file.
  if(r.reuse&&r.reuse->atlas&&strcmp(r.reuse->root,r.root)==0) {
    pack->atlas=r.reuse->atlas;pack->atlasHeader=r.reuse->atlasHeader;
  } else {
    pack->atlas=std::make_shared<Blob>();
    if(!image(atlasName,*pack->atlas,pack->atlasHeader,3)||
       pack->atlasHeader.width>320||pack->atlasHeader.height>320) return fail("atlas invalid (max 320px, 16 frames)");
  }
  const char *bgName=schema==2?doc["backgrounds"][pack->selected]["image"].as<const char *>():
    doc["background"].as<const char *>();
  if(!image(bgName,pack->background,pack->backgroundHeader,2)||
     pack->backgroundHeader.width!=480||pack->backgroundHeader.height!=480||
     pack->backgroundHeader.frames!=1) return fail("background must be 480x480");
  if(schema==2) for(unsigned i=0;i<backgrounds;i++) {
    auto &h=pack->thumbHeaders[i];
    if(!image(doc["backgrounds"][i]["thumb"],pack->thumbs[i],h,2)||h.width>160||h.height>160||h.frames!=1)
      return fail("background thumbnail invalid (max 160px)");
  }
  for(unsigned i=0;i<4;i++) {
    const char *name=doc["sounds"][sounds[i]];
    if(!game::safeName(name)) return fail("missing sound slot");
    const char *hash=doc["files"][name]["sha256"];
    Blob &b=pack->sounds[i];
    if(!hash||!readBlob(String(r.root)+"/"+name,b,160044,hash)||b.size<44||
       b.size!=doc["files"][name]["bytes"].as<size_t>()) return fail("sound read/hash failed");
    const auto p=b.data;
    auto u32=[p](unsigned o){return uint32_t(p[o])|uint32_t(p[o+1])<<8|uint32_t(p[o+2])<<16|uint32_t(p[o+3])<<24;};
    if(memcmp(p,"RIFF",4)||memcmp(p+8,"WAVEfmt ",8)||u32(16)!=16||p[20]!=1||p[21]||
       p[22]!=1||p[23]||u32(24)!=16000||p[34]!=16||p[35]||memcmp(p+36,"data",4)||
       u32(4)!=b.size-8||u32(28)!=32000||p[32]!=2||p[33]||
       u32(40)!=b.size-44||((b.size-44)%2)) return fail("sound must be canonical PCM WAV 16k mono");
  }
  const char *clipError=nullptr;
  auto parse=[&](JsonObject c,Clip &clip) {
    const size_t count=c["frames"].size();
    if(!count||count>32||c["durations"].size()!=count) {clipError="clip frame count invalid";return false;}
    clip.count=count;clip.loop=c["loop"]|false;
    for(unsigned j=0;j<count;j++) {
      if(!c["frames"][j].is<unsigned>()||!c["durations"][j].is<unsigned>()) {clipError="clip types invalid";return false;}
      unsigned frame=c["frames"][j],ms=c["durations"][j];
      if(frame>=pack->atlasHeader.frames||ms<16||ms>5000) {clipError="clip frame/time out of bounds";return false;}
      clip.frames[j]=frame;clip.durations[j]=ms;
    }
    const char *name=c["sound"]|"";
    for(int j=0;j<4;j++) if(strcmp(name,sounds[j])==0) clip.sound=j;
    if(*name&&clip.sound<0) {clipError="clip sound unknown";return false;}
    return true;
  };
  for(unsigned i=0;i<8;i++) if(!parse(doc["clips"][actionNames[i]],pack->clips[i])) return fail(clipError);
  // Optional: random tap reactions, ambient walk and sit.
  JsonArray taps=doc["taps"];
  if(taps.size()>kMaxTaps) return fail("at most 6 tap reactions");
  for(JsonObject t:taps) if(!parse(t,pack->taps[pack->tapCount++])) return fail(clipError);
  if(doc["walk"].is<JsonObject>()&&!parse(doc["walk"],pack->walk)) return fail(clipError);
  if(doc["sit"].is<JsonObject>()&&!parse(doc["sit"],pack->sit)) return fail(clipError);
  result.pack=pack.release();return true;
}
bool Assets::verifyPack(const char *root,String &error) {
  unsigned count=1;
  std::unique_ptr<AssetPack> first;  // verified atlas, shared by the other backgrounds' checks
  for(uint8_t background=0;background<count;background++) {
    Request r{};snprintf(r.root,sizeof(r.root),"%s",root);r.background=background;r.reuse=first.get();
    AssetResult result;
    if(!load(r,result)) {error=result.error;return false;}
    count=result.pack->backgroundCount;
    if(first) delete result.pack;
    else first.reset(result.pack);
  }
  return true;
}
bool PackTransfer::start(const char *name,String &error) {
  if(!assets.ready||active||!game::safeName(name)||strlen(name)>32) {error="SD unavailable, busy, or invalid pack name";return false;}
  SdGuard lock;
  if(!SD.exists("/chimmygochi")&&!SD.mkdir("/chimmygochi")) {error="cannot create app directory";return false;}
  if(!SD.exists("/chimmygochi/packs")&&!SD.mkdir("/chimmygochi/packs")) {error="cannot create packs directory";return false;}
  folder=String("/chimmygochi/packs/")+name;
  if(SD.exists(folder)||!SD.mkdir(folder)) {error="pack exists or mkdir failed; no data overwritten";return false;}
  active=true;lastActivity=millis();return true;
}
bool PackTransfer::open(const char *name,size_t size,const char *hash,String &error) {
  if(!active||file||!game::safeName(name)||!size||size>4*1024*1024+16||!hash||strlen(hash)!=64) {
    error="invalid file request";return false;}
  for(const char *p=hash;*p;p++) if(!isxdigit(*p)) {error="invalid SHA256";return false;}
  SdGuard lock;path=folder+"/"+name;
  if(SD.exists(path)||SD.exists(path+".part")) {error="file exists; no overwrite";return false;}
  file=SD.open(path+".part",FILE_WRITE);
  if(!file) {error="file open failed";return false;}
  expected=size;received=0;expectedHash=hash;lastActivity=millis();
  if(mbedtls_sha256_starts(&sha,0)!=0) {file.close();error="SHA init failed";return false;}
  return true;
}
bool PackTransfer::chunk(size_t offset,const char *hex,String &error) {
  const size_t len=hex?strlen(hex):0;
  if(!active||!file||offset!=received||!len||len%2||len>2048||received+len/2>expected) {
    Serial.printf("ERROR chunk offset=%u received=%u hex=%u expected=%u active=%d file=%d\n",
      unsigned(offset),unsigned(received),unsigned(len),unsigned(expected),active,bool(file));
    error="invalid chunk/offset";return false;}
  uint8_t data[1024];
  auto nib=[](char c)->int{if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;return -1;};
  for(size_t i=0;i<len/2;i++) {
    int a=nib(hex[i*2]),b=nib(hex[i*2+1]);if(a<0||b<0) {error="invalid hex";return false;}
    data[i]=(a<<4)|b;
  }
  {SdGuard lock;if(file.write(data,len/2)!=len/2) {error="SD short write";return false;}}
  if(mbedtls_sha256_update(&sha,data,len/2)!=0) {error="SHA update failed";return false;}
  received+=len/2;lastActivity=millis();return true;
}
bool PackTransfer::finish(String &error) {
  if(!file||received!=expected) {error="incomplete transfer";return false;}
  uint8_t digest[32];char hex[65]{};
  if(mbedtls_sha256_finish(&sha,digest)!=0) {error="SHA finish failed";return false;}
  for(unsigned i=0;i<32;i++) snprintf(hex+2*i,3,"%02x",digest[i]);
  SdGuard lock;file.flush();file.close();
  if(expectedHash!=hex) {error="checksum mismatch; partial file retained";return false;}
  if(!SD.rename(path+".part",path)) {error="rename failed";return false;}
  lastActivity=millis();return true;
}
bool PackTransfer::complete(String &error) {
  if(!active||file) {error="transfer not complete";return false;}
  if(!assets.verifyPack(folder.c_str(),error)) return false;
  if(!assets.activate(folder.c_str())) {error="active pointer save failed";return false;}
  active=false;return true;
}
void PackTransfer::abort() {
  if(file) {SdGuard lock;file.close();}
  active=false;
}
