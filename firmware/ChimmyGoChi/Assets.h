#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <SD.h>
#include <SPI.h>
#include <lvgl.h>
#include <mbedtls/sha256.h>
#include <memory>
#include "GameModel.h"

constexpr const char *actionNames[]={"idle","tap","eat","play","sleep","dress","refuse","happy"};
struct Clip {
  uint8_t frames[32]{};
  uint16_t durations[32]{};
  uint8_t count=0;
  bool loop=false;
  int8_t sound=-1;
};
struct Blob {
  uint8_t *data=nullptr;
  size_t size=0;
  ~Blob(){free(data);}
};
constexpr unsigned kMaxBackgrounds=8,kMaxTaps=6;
struct AssetPack {
  // Shared so a background change can keep the (multi-MB) character atlas instead of loading a copy.
  std::shared_ptr<Blob> atlas;
  Blob background,sounds[4],thumbs[kMaxBackgrounds];
  game::ImageHeader atlasHeader{},backgroundHeader{},thumbHeaders[kMaxBackgrounds]{};
  Clip clips[8],taps[kMaxTaps],walk,sit;  // taps: random reactions; walk/sit: optional (count 0)
  uint8_t tapCount=0;
  bool placeholder=true;
  uint8_t backgroundCount=1,selected=0;  // selected: index of the loaded background
  char root[96]{};
};
struct AssetResult {
  AssetPack *pack=nullptr;
  uint32_t generation=0;
  char error[128]{};
};
class Assets {
  SPIClass spi{HSPI};
  QueueHandle_t requests=nullptr,results=nullptr;
  // reuse: the pack on screen; it stays alive until this request's result is consumed.
  struct Request {char root[96];uint8_t background;uint32_t generation;const AssetPack *reuse;};
  static void worker(void *);
  bool load(const Request &,AssetResult &);
  bool readBlob(const String &,Blob &,size_t,const char *);
 public:
  SemaphoreHandle_t mutex=nullptr;
  bool ready=false;
  bool begin();
  bool request(const char *root,uint8_t background,uint32_t generation,const AssetPack *reuse=nullptr);
  bool poll(AssetResult &r);
  String activeRoot();
  bool activate(const char *root);
  bool verifyPack(const char *root,String &error);
  // Diagnostic: reads `count` chunks of `chunk` bytes from `path` (no hashing); 0 on error.
  uint32_t benchRead(const char *path,size_t chunk,unsigned count);
};
extern Assets assets;

class PackTransfer {
  File file;
  String folder,path,expectedHash;
  size_t expected=0,received=0;
  mbedtls_sha256_context sha{};
 public:
  bool active=false;
  uint32_t lastActivity=0;
  PackTransfer(){mbedtls_sha256_init(&sha);}
  ~PackTransfer(){mbedtls_sha256_free(&sha);}
  bool start(const char *name,String &error);
  bool open(const char *name,size_t size,const char *hash,String &error);
  bool chunk(size_t offset,const char *hex,String &error);
  bool finish(String &error);
  bool complete(String &error);
  void abort();
};
