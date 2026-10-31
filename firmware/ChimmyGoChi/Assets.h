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
struct AssetPack {
  Blob atlas,background,sounds[4];
  game::ImageHeader atlasHeader{},backgroundHeader{};
  Clip clips[8];
  char names[3][40]{};
  bool placeholder=true;
  uint8_t outfit=0;
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
  struct Request {char root[96];uint8_t outfit;uint32_t generation;};
  static void worker(void *);
  bool load(const Request &,AssetResult &);
  bool readBlob(const String &,Blob &,size_t,const char *);
 public:
  SemaphoreHandle_t mutex=nullptr;
  bool ready=false;
  bool begin();
  bool request(const char *root,uint8_t outfit,uint32_t generation);
  bool poll(AssetResult &r);
  String activeRoot();
  bool activate(const char *root);
  bool verifyPack(const char *root,String &error);
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
