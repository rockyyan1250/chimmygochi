#pragma once
#include <nvs.h>
#include <nvs_flash.h>
#include "GameModel.h"

class SaveStore {
  nvs_handle_t handle=0;
  bool opened=false;
 public:
  enum class Load { New, Ready, Recovered, Corrupt, Unavailable };
  bool begin() {
    esp_err_t err=nvs_flash_init_partition("gamesave");
    if(err==ESP_OK) err=nvs_open_from_partition("gamesave","pet",NVS_READWRITE,&handle);
    opened=err==ESP_OK;
    if(!opened) Serial.printf("ERROR save init %s\n",esp_err_to_name(err));
    return opened;
  }
  Load load(game::State &s) {
    if(!opened) return Load::Unavailable;
    game::State slots[2]; bool valid[2]{},found[2]{};
    for(unsigned i=0;i<2;i++) {
      game::Record r{};size_t len=r.size();
      const auto err=nvs_get_blob(handle,i?"save_b":"save_a",r.data(),&len);
      found[i]=err!=ESP_ERR_NVS_NOT_FOUND;
      valid[i]=err==ESP_OK&&len==r.size()&&game::decode(r,slots[i]);
    }
    if(!found[0]&&!found[1]) return Load::New;
    if(!valid[0]&&!valid[1]) return Load::Corrupt;
    s=valid[0]&&(!valid[1]||slots[0].generation>=slots[1].generation)?slots[0]:slots[1];
    return (found[0]&&!valid[0])||(found[1]&&!valid[1])?Load::Recovered:Load::Ready;
  }
  bool save(game::State &s,int64_t utc) {
    if(!opened||s.generation==UINT64_MAX) return false;
    auto next=s;next.savedUtc=game::validUtc(utc)?utc:0;next.generation++;
    const auto r=game::encode(next);const char *key=(next.generation&1)?"save_a":"save_b";
    esp_err_t err=nvs_set_blob(handle,key,r.data(),r.size());
    if(err==ESP_OK) err=nvs_commit(handle);
    game::Record read{};size_t len=read.size();
    if(err==ESP_OK) err=nvs_get_blob(handle,key,read.data(),&len);
    if(err!=ESP_OK||len!=r.size()||read!=r) {
      Serial.printf("ERROR save commit/verify %s\n",esp_err_to_name(err));return false;
    }
    s.generation=next.generation;s.savedUtc=next.savedUtc;return true;
  }
  bool reset(game::State &s,int64_t utc) {
    if(!opened) return false;
    esp_err_t err=nvs_erase_all(handle);
    if(err==ESP_OK)err=nvs_commit(handle);
    if(err!=ESP_OK){Serial.printf("ERROR save reset %s\n",esp_err_to_name(err));return false;}
    s=game::State{};
    return save(s,utc)&&save(s,utc);
  }
};
