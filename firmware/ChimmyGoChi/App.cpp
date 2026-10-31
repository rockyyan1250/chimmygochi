#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <esp_timer.h>
#include <sys/time.h>
#include <cmath>
#include <memory>
#include "Board.h"
#include "SaveStore.h"
#include "Assets.h"
#include "UiText.h"

LV_FONT_DECLARE(font_tc);
LV_FONT_DECLARE(font_clock);
static game::State pet;
static SaveStore saves;
static bool saveLocked=false,dirty=false,assetLoading=false;
static uint32_t dirtyAt=0,lastSave=0,sceneGeneration=0;
static uint64_t lastTickUs=0;
static int page=1,action=0,clockQuality=0;
static uint32_t actionAt=0,toastUntil=0,frameCount=0,maxLoopUs=0;
static std::unique_ptr<AssetPack> pack;
static PackTransfer transfer;
static lv_obj_t *screen=nullptr,*character=nullptr,*background=nullptr,*toast=nullptr;
static lv_obj_t *clockLabel=nullptr,*dateLabel=nullptr,*infoLabel=nullptr,*stats[3]{};
static lv_obj_t *batteryLabel=nullptr;
static battery::Reading batteryReading{battery::State::Unavailable,-1};
static bool batterySampled=false;
static uint32_t batteryReadAt=0;
static lv_obj_t *fallback=nullptr,*eyes[2]{},*fallbackBody=nullptr;
static lv_img_dsc_t atlasImage{},bgImage{};
static uint32_t lastFrame=UINT32_MAX,lastFrameAt=0;
static char toastMessage[200]{};
static tm timeEdit{};
static lv_obj_t *timeFields[5]{};
static int editField=0;
static bool networkBusy=false;
static uint32_t networkAt=0;
static bool bubbleGame=false;
static unsigned bubbleScore=0;
static lv_obj_t *bubble=nullptr,*bubbleLabel=nullptr;
static uint32_t bubbleAt=0;
static uint32_t audioFailureSeen=0;
static bool urgentSave=false,capture=false;
static uint32_t captureAt=0;

enum Page { Game=0,Ambient=1,Clock=2,Settings=3,Foods=4,Toys=5,Outfits=6,Status=7,SetTime=8 };
enum Control { GoGame=1,GoAmbient,GoClock,GoSettings,GoFoods,GoToys,GoOutfits,Sleep,
  GoStatus,Back,Tap,Feed0=20,Play0=30,Outfit0=40,BrightnessDown=50,BrightnessUp,VolumeDown,VolumeUp,
  AutoOff,Hour24,NightMute,StartMode,OfflineCap,EditTime,TimePrev,TimeNext,TimeMinus,TimePlus,TimeSave,
  Reload,Sync,Bubble,ResetConfirm,ResetYes };
static void showPage(int next);
static void notify(const char *message,uint32_t duration=4000);
static void setAction(int next);
static void control(int id);
static int64_t nowUtc() {const int64_t t=time(nullptr);return game::validUtc(t)?t:0;}
static void advanceNow() {
  const uint64_t now=esp_timer_get_time();
  const uint64_t seconds=(now-lastTickUs)/1000000;
  if(seconds) {
    game::advance(pet,std::min<uint64_t>(seconds,UINT32_MAX));lastTickUs+=seconds*1000000;
    if(!dirty) {dirty=true;dirtyAt=millis();}
  }
}
static bool saveNow() {
  if(saveLocked) return false;
  advanceNow();
  const bool ok=saves.save(pet,nowUtc());
  if(ok) {dirty=false;urgentSave=false;lastSave=millis();}
  else notify("保存失敗：請檢查裝置",10000);
  return ok;
}
static void changed() {dirtyAt=millis();dirty=true;urgentSave=true;}
static lv_color_t color(uint32_t value) {return lv_color_hex(value);}
static lv_obj_t *label(lv_obj_t *parent,const char *value,int x,int y,int width,uint32_t ink=0x304057) {
  auto l=lv_label_create(parent);lv_label_set_text(l,value);
  lv_obj_set_pos(l,x,y);lv_obj_set_width(l,width);
  lv_obj_set_style_text_font(l,&font_tc,0);lv_obj_set_style_text_color(l,color(ink),0);
  return l;
}
static void initBatteryIndicator() {
  batteryLabel=label(lv_layer_top(),"電量 --",220,7,138);
  lv_obj_set_style_text_align(batteryLabel,LV_TEXT_ALIGN_CENTER,0);
  lv_obj_set_style_pad_all(batteryLabel,3,0);
  lv_obj_set_style_radius(batteryLabel,8,0);
  lv_obj_set_style_bg_opa(batteryLabel,LV_OPA_COVER,0);
  lv_obj_clear_flag(batteryLabel,LV_OBJ_FLAG_CLICKABLE);
}
static void updateBattery() {
  const uint32_t now=millis();
  if(batterySampled&&now-batteryReadAt<1000)return;
  const auto next=readBattery();
  if(next.state==battery::State::Unavailable&&
     (!batterySampled||batteryReading.state!=battery::State::Unavailable))
    Serial.println("ERROR battery reading unavailable: check PMIC, detection and fuel gauge");
  if(!batterySampled||next.state!=batteryReading.state||next.percent!=batteryReading.percent) {
    batteryReading=next;
    char text[32];battery::format(batteryReading,text,sizeof(text));
    lv_label_set_text(batteryLabel,text);
    const uint32_t bg=next.state==battery::State::Disconnected?0xedece8:
      next.state==battery::State::Unavailable||next.percent<=20?0xffe1d8:0xe0f3e8;
    lv_obj_set_style_bg_color(batteryLabel,color(bg),0);
  }
  batteryReadAt=now;batterySampled=true;
}
static void event(lv_event_t *e) {
  if(lv_event_get_code(e)!=LV_EVENT_CLICKED) return;
  lastInput=millis();
  control(static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e))));
}
static lv_obj_t *button(lv_obj_t *parent,const char *value,int x,int y,int w,int h,int id,uint32_t bg=0xffffff) {
  auto b=lv_btn_create(parent);lv_obj_set_pos(b,x,y);lv_obj_set_size(b,w,h);
  lv_obj_set_style_radius(b,16,0);lv_obj_set_style_bg_color(b,color(bg),0);
  lv_obj_set_style_shadow_width(b,0,0);lv_obj_set_style_border_width(b,0,0);
  lv_obj_add_event_cb(b,event,LV_EVENT_CLICKED,reinterpret_cast<void *>(static_cast<intptr_t>(id)));
  auto l=label(b,value,0,0,w-8);lv_obj_set_style_text_align(l,LV_TEXT_ALIGN_CENTER,0);lv_obj_center(l);
  return b;
}
static void notify(const char *message,uint32_t duration) {
  snprintf(toastMessage,sizeof(toastMessage),"%s",message);toastUntil=millis()+duration;
  Serial.printf("NOTICE %s\n",message);
  if(toast) {lv_label_set_text(toast,toastMessage);lv_obj_clear_flag(toast,LV_OBJ_FLAG_HIDDEN);}
}
static bool elapsed(uint32_t deadline) {return int32_t(millis()-deadline)>=0;}
static void requestAssets() {
  if(assetLoading||transfer.active) {notify("素材載入中");return;}
  const String root=assets.activeRoot();
  if(!assets.ready||root.isEmpty()) {notify("示範素材：使用內建圖形，請由USB安裝素材",8000);return;}
  assetLoading=assets.request(root.c_str(),pet.outfit,++sceneGeneration);
  if(!assetLoading) notify("素材載入失敗");
  else notify("載入中");
}
static bool muteNow() {
  if(!pet.volume||!displayOn) return true;
  if(!pet.nightMute) return false;
  int64_t t=nowUtc();if(!t) return true;
  time_t epoch=t;tm local{};localtime_r(&epoch,&local);
  return local.tm_hour>=22||local.tm_hour<8;
}
static void soundFor(int next) {
  if(muteNow()||!pack) return;
  const int sound=pack->clips[next].sound;
  if(sound>=0 && !playSound(pack->sounds[sound].data,pack->sounds[sound].size,pet.volume))
    notify("音效播放失敗");
}
static void setAction(int next) {
  stopSound();action=next;actionAt=millis();lastFrame=UINT32_MAX;
  soundFor(next);
}
static void makeScene() {
  if(pack) {
    bgImage={};bgImage.header.cf=LV_IMG_CF_TRUE_COLOR;bgImage.header.w=480;bgImage.header.h=480;
    bgImage.data=pack->background.data+16;bgImage.data_size=480*480*2;
    background=lv_img_create(screen);lv_img_set_src(background,&bgImage);lv_obj_set_pos(background,0,0);
    character=lv_img_create(screen);lv_obj_set_pos(character,(480-pack->atlasHeader.width)/2,80);
    lv_obj_add_flag(character,LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(character,event,LV_EVENT_CLICKED,reinterpret_cast<void *>(static_cast<intptr_t>(Tap)));
  } else {
    fallback=lv_obj_create(screen);lv_obj_set_size(fallback,190,190);lv_obj_set_pos(fallback,145,105);
    lv_obj_clear_flag(fallback,LV_OBJ_FLAG_SCROLLABLE);lv_obj_set_style_radius(fallback,75,0);
    const uint32_t palette[]={0x6bd8d0,0xada4ed,0xf5a8bf};
    lv_obj_set_style_bg_color(fallback,color(palette[pet.outfit]),0);lv_obj_set_style_border_width(fallback,4,0);
    lv_obj_set_style_border_color(fallback,color(0xffffff),0);
    for(unsigned i=0;i<2;i++) {
      eyes[i]=lv_obj_create(fallback);lv_obj_set_size(eyes[i],12,18);lv_obj_set_pos(eyes[i],45+i*65,60);
      lv_obj_set_style_bg_color(eyes[i],color(0x304057),0);lv_obj_set_style_radius(eyes[i],8,0);lv_obj_set_style_border_width(eyes[i],0,0);
    }
    fallbackBody=label(fallback,"~",66,88,40);lv_obj_set_style_text_font(fallbackBody,&lv_font_montserrat_48,0);
    lv_obj_add_event_cb(fallback,event,LV_EVENT_CLICKED,reinterpret_cast<void *>(static_cast<intptr_t>(Tap)));
  }
}
static void header(const char *title) {
  auto bar=lv_obj_create(screen);lv_obj_set_pos(bar,0,0);lv_obj_set_size(bar,480,56);
  lv_obj_set_style_radius(bar,0,0);lv_obj_set_style_bg_opa(bar,LV_OPA_90,0);
  lv_obj_set_style_bg_color(bar,color(0xfffaf3),0);lv_obj_set_style_border_width(bar,0,0);
  lv_obj_clear_flag(bar,LV_OBJ_FLAG_SCROLLABLE);
  label(screen,title,18,14,195);
  if(page<=2) button(screen,pet.sleeping?"起床":"睡覺",365,5,104,46,Sleep,0xede7fb);
  else button(screen,"返回",365,5,104,46,Back,0xede7fb);
}
static void footer() {
  const char *names[]={"遊戲","待機","時鐘","設定"};
  const int ids[]={GoGame,GoAmbient,GoClock,GoSettings};
  for(int i=0;i<4;i++) button(screen,names[i],8+i*118,421,110,51,ids[i],page==i?0xb7e6df:0xf7f3ed);
}
static void showPage(int next) {
  auto oldScreen=screen;
  screen=lv_obj_create(nullptr);
  lv_obj_clear_flag(screen,LV_OBJ_FLAG_SCROLLABLE);lv_obj_set_style_bg_color(screen,color(0xf0f0ec),0);
  lv_scr_load(screen);
  if(oldScreen)lv_obj_del(oldScreen);
  page=next;character=background=fallback=fallbackBody=clockLabel=dateLabel=infoLabel=bubble=bubbleLabel=nullptr;
  toast=nullptr;for(auto &p:stats)p=nullptr;
  bubbleGame=false;stopSound();action=pet.sleeping?4:0;actionAt=millis();lastFrame=UINT32_MAX;
  if(page==Game||page==Ambient) {
    makeScene();header("CHIMMY GOCHI");
    label(screen,(!pack||pack->placeholder)?"示範素材 / 非官方圖片":"BABY CHIMMY",18,64,440,0x68556a);
    if(page==Game) {
      const char *titles[]={"飽足","心情","體力"};
      for(int i=0;i<3;i++) {
        stats[i]=label(screen,titles[i],16+i*157,304,150);
        lv_obj_set_style_bg_color(stats[i],color(0xfffaf3),0);lv_obj_set_style_bg_opa(stats[i],LV_OPA_90,0);
      }
      button(screen,"餵食",12,354,146,56,GoFoods,0xffddb5);
      button(screen,"玩耍",167,354,146,56,GoToys,0xc0e5e2);
      button(screen,"換裝",322,354,146,56,GoOutfits,0xded6f5);
    } else {
      clockLabel=label(screen,"--:--",163,345,200);
      lv_obj_set_style_text_font(clockLabel,&lv_font_montserrat_48,0);
      button(screen,"狀態",18,357,110,50,GoStatus,0xffffff);
    }
    footer();
  } else if(page==Clock) {
    lv_obj_set_style_bg_color(screen,color(0x111923),0);header("CHIMMY / 時鐘");
    clockLabel=label(screen,"--:--",90,145,320,0xf7e4b7);
    lv_obj_set_style_text_font(clockLabel,&font_clock,0);
    dateLabel=label(screen,"請設定時間",50,270,400,0xc5d7d9);
    infoLabel=label(screen,"",50,315,400,0xa5b7ba);footer();
  } else if(page==Foods||page==Toys||page==Outfits) {
    header(page==Foods?"餵食":page==Toys?"玩耍":"換裝");
    const unsigned count=page==Foods?6:page==Toys?2:3;
    for(unsigned i=0;i<count;i++) {
      const char *name=page==Foods?text::foods[i]:page==Toys?text::toys[i]:pack?pack->names[i]:text::outfits[i];
      button(screen,name,18+(i%2)*231,105+(i/2)*100,213,82,
        (page==Foods?Feed0:page==Toys?Play0:Outfit0)+i,
        page==Foods?0xffe2bd:page==Toys?0xc0e5e2:0xded6f5);
    }
  } else if(page==Settings) {
    header("設定");
    auto panel=lv_obj_create(screen);lv_obj_set_pos(panel,0,64);lv_obj_set_size(panel,480,410);
    lv_obj_set_style_pad_all(panel,0,0);lv_obj_set_style_border_width(panel,0,0);
    label(panel,"亮度",20,10,140);button(panel,"-",230,0,100,55,BrightnessDown);button(panel,"+",345,0,100,55,BrightnessUp);
    label(panel,"音量",20,78,140);button(panel,"-",230,68,100,55,VolumeDown);button(panel,"+",345,68,100,55,VolumeUp);
    char tmp[80];
    snprintf(tmp,sizeof(tmp),"無操作關屏：%s",pet.autoOff?"開":"關");button(panel,tmp,15,140,445,60,AutoOff);
    snprintf(tmp,sizeof(tmp),"夜間靜音：%s",pet.nightMute?"開":"關");button(panel,tmp,15,210,445,60,NightMute);
    button(panel,pet.hour24?"24 小時":"12 小時",15,280,445,60,Hour24);
    snprintf(tmp,sizeof(tmp),"啟動畫面：%s",text::modes[int(pet.startMode)]);button(panel,tmp,15,350,445,60,StartMode);
    snprintf(tmp,sizeof(tmp),"離線計算上限：%u 小時",pet.offlineHours);button(panel,tmp,15,420,445,60,OfflineCap);
    button(panel,"設定日期時間",15,490,445,60,EditTime);
    button(panel,"同步網絡時間",15,560,445,60,Sync);
    button(panel,"重新載入素材",15,630,445,60,Reload);
    button(panel,"狀態 / 版本",15,700,445,60,GoStatus);
    label(panel,chargingDisabled?"充電停用：待確認電池規格":"充電狀態未確認：請拔除電池",18,780,440);
    if(saveLocked) button(panel,"資料損壞：確認重設",15,835,445,60,ResetConfirm,0xffc7c7);
  } else if(page==Status) {
    header("狀態");
    infoLabel=label(screen,"",24,105,435);
    button(screen,"返回遊戲",20,410,440,60,GoGame,0xc0e5e2);
  } else if(page==SetTime) {
    header("設定日期時間");
    label(screen,"香港時間 UTC+8",30,86,400);
    for(unsigned i=0;i<5;i++) timeFields[i]=label(screen,"",30+i*89,155,85);
    button(screen,"<",20,230,100,60,TimePrev);
    button(screen,">",130,230,100,60,TimeNext);
    button(screen,"-",250,230,100,60,TimeMinus);
    button(screen,"+",360,230,100,60,TimePlus);
    button(screen,"確定",30,330,420,65,TimeSave,0xc0e5e2);
  }
  toast=label(screen,toastMessage,12,65,456,0x594761);
  lv_obj_set_style_bg_color(toast,color(0xfff0cf),0);lv_obj_set_style_bg_opa(toast,LV_OPA_90,0);
  if(elapsed(toastUntil)) lv_obj_add_flag(toast,LV_OBJ_FLAG_HIDDEN);
}
static void resultMessage(game::Result result) {
  if(result==game::Result::Sleeping) notify("請先起床");
  else if(result==game::Result::Full) notify("吃飽了");
  else if(result==game::Result::Tired) notify("需要休息");
  else notify("操作無效");
}
static bool canAct() {
  if(saveLocked) {notify("資料損壞：請先復原");return false;}
  if(transfer.active||assetLoading) {notify("載入中");return false;}
  if(action!=0&&action!=4) {notify("請稍候");return false;}
  return true;
}
static bool setTime(int64_t epoch) {
  if(!game::validUtc(epoch)) return false;
  advanceNow();
  timeval tv{time_t(epoch),0};
  if(settimeofday(&tv,nullptr)!=0) return false;
  clockQuality=2;
  const bool rtc=rtcWrite(epoch);
  changed();
  if(!saveLocked&&!saveNow()) return false;
  notify(rtc?"時間已設定":"時間已設定，RTC寫入失敗",6000);
  return true;
}
static void syncTime() {
  if(networkBusy) {notify("連接網絡中");return;}
  WiFi.mode(WIFI_STA);WiFi.begin();networkBusy=true;networkAt=millis();notify("連接網絡中");
}
static void control(int id) {
  if(transfer.active) {notify("USB素材傳輸中");return;}
  if(id>=Feed0&&id<Feed0+6) {
    if(!canAct()) return;advanceNow();
    const auto result=game::feed(pet,id-Feed0);
    if(result!=game::Result::Ok) {resultMessage(result);return;}
    changed();showPage(Game);setAction(2);notify("好開心！");return;
  }
  if(id>=Play0&&id<Play0+2) {
    if(!canAct()) return;
    if(pet.sleeping) {notify("請先起床");return;}
    if(pet.energy<800) {notify("需要休息");return;}
    if(id==Play0) {
      advanceNow();const auto result=game::play(pet,0);
      if(result!=game::Result::Ok){resultMessage(result);return;}
      changed();showPage(Game);setAction(3);
    } else {
      showPage(Toys);bubbleGame=true;bubbleScore=0;bubbleAt=millis();
      lv_obj_clean(screen);toast=nullptr;header("戳泡泡");
      bubbleLabel=label(screen,"分數 0 / 5",30,90,420);
      bubble=button(screen,"泡泡",170,190,110,110,Bubble,0xc0e5e2);
    }
    return;
  }
  if(id>=Outfit0&&id<Outfit0+3) {
    if(!canAct()) return;
    pet.outfit=id-Outfit0;changed();showPage(Game);requestAssets();setAction(5);return;
  }
  switch(id) {
    case GoGame:showPage(Game);break;
    case GoAmbient:showPage(Ambient);break;
    case GoClock:showPage(Clock);break;
    case GoSettings:showPage(Settings);break;
    case GoFoods:if(canAct())showPage(Foods);break;
    case GoToys:if(canAct())showPage(Toys);break;
    case GoOutfits:if(canAct())showPage(Outfits);break;
    case GoStatus:showPage(Status);break;
    case Tap:
      if(page<=Ambient&&canAct()) {
        setAction(pet.sleeping?4:pet.bond>=40?7:1);
        notify(pet.sleeping?"晚安":pet.bond>=40?"新反應已解鎖！":"好開心！");
      }
      break;
    case Back:showPage(page==Status||page==SetTime?Settings:Game);break;
    case Sleep:
      if(!canAct())break;advanceNow();pet.sleeping=!pet.sleeping;pet.sleepSeconds=0;changed();
      showPage(page);notify(pet.sleeping?"晚安":"起床了");break;
    case BrightnessDown:pet.brightness=std::max<int>(20,pet.brightness-20);changed();display.setBrightness(pet.brightness);break;
    case BrightnessUp:pet.brightness=std::min<int>(220,pet.brightness+20);changed();display.setBrightness(pet.brightness);break;
    case VolumeDown:pet.volume=std::max<int>(0,pet.volume-10);changed();break;
    case VolumeUp:pet.volume=std::min<int>(100,pet.volume+10);changed();soundFor(1);break;
    case AutoOff:pet.autoOff=!pet.autoOff;changed();showPage(Settings);break;
    case Hour24:pet.hour24=!pet.hour24;changed();showPage(Settings);break;
    case NightMute:pet.nightMute=!pet.nightMute;changed();showPage(Settings);break;
    case StartMode:pet.startMode=game::Mode((int(pet.startMode)+1)%3);changed();showPage(Settings);break;
    case OfflineCap:pet.offlineHours=pet.offlineHours==1?6:pet.offlineHours==6?12:1;changed();showPage(Settings);break;
    case EditTime: {
      time_t t=nowUtc();
      if(t) localtime_r(&t,&timeEdit);
      else {timeEdit={};timeEdit.tm_year=126;timeEdit.tm_mon=0;timeEdit.tm_mday=1;timeEdit.tm_hour=12;}
      editField=0;showPage(SetTime);break;
    }
    case TimePrev:editField=(editField+4)%5;break;
    case TimeNext:editField=(editField+1)%5;break;
    case TimeMinus:case TimePlus:{
      int *fields[]={&timeEdit.tm_year,&timeEdit.tm_mon,&timeEdit.tm_mday,&timeEdit.tm_hour,&timeEdit.tm_min};
      const int lo[]={124,0,1,0,0},hi[]={199,11,31,23,59};
      *fields[editField]+=id==TimePlus?1:-1;
      if(*fields[editField]>hi[editField])*fields[editField]=lo[editField];
      if(*fields[editField]<lo[editField])*fields[editField]=hi[editField];break;
    }
    case TimeSave:{
      tm t=timeEdit;t.tm_sec=0;t.tm_isdst=0;const time_t epoch=mktime(&t);
      if(t.tm_mday!=timeEdit.tm_mday||t.tm_mon!=timeEdit.tm_mon||!setTime(epoch)) notify("日期無效或保存失敗");
      else showPage(Clock);break;
    }
    case Reload:requestAssets();break;
    case Sync:syncTime();break;
    case Bubble:
      if(!bubbleGame)break;
      if(++bubbleScore>=5) {
        advanceNow();const auto result=game::play(pet,1);bubbleGame=false;
        if(result==game::Result::Ok) {changed();showPage(Game);setAction(7);notify("完成！");}
        else {showPage(Game);resultMessage(result);}
      } else {
        char score[48];snprintf(score,sizeof(score),"分數 %u / 5",bubbleScore);lv_label_set_text(bubbleLabel,score);
        lv_obj_set_pos(bubble,30+esp_random()%320,145+esp_random()%190);
      }
      break;
    case ResetConfirm:
      showPage(Status);button(screen,"重設育成資料？",20,320,440,65,ResetYes,0xffc7c7);break;
    case ResetYes:
      if(!saveLocked)break;
      if(!saves.reset(pet,nowUtc())){notify("重設失敗：請檢查裝置",10000);break;}
      saveLocked=false;dirty=false;urgentSave=false;lastSave=millis();lastTickUs=esp_timer_get_time();
      showPage(Game);notify("資料已重設");break;
  }
}
static void updateAnimation() {
  if((page!=Game&&page!=Ambient)||!displayOn||transfer.active) return;
  const uint32_t delta=millis()-actionAt;
  if(pack&&character) {
    const Clip &c=pack->clips[action];uint32_t total=0;for(unsigned i=0;i<c.count;i++)total+=c.durations[i];
    if(!total)return;
    if(!c.loop&&delta>=total) {setAction(pet.sleeping?4:0);return;}
    uint32_t phase=delta%total;unsigned index=0;
    while(index+1<c.count&&phase>=c.durations[index]) {phase-=c.durations[index];index++;}
    const uint32_t frame=c.frames[index];
    if(frame!=lastFrame) {
      atlasImage={};atlasImage.header.cf=LV_IMG_CF_TRUE_COLOR_ALPHA;
      atlasImage.header.w=pack->atlasHeader.width;atlasImage.header.h=pack->atlasHeader.height;
      atlasImage.data_size=pack->atlasHeader.width*pack->atlasHeader.height*3;
      atlasImage.data=pack->atlas.data+16+frame*atlasImage.data_size;
      lv_img_cache_invalidate_src(&atlasImage);lv_img_set_src(character,&atlasImage);
      lastFrame=frame;frameCount++;
    }
    const int base=page==Game?76:94;
    const int bounce=int(std::sin(delta/220.0)*(action==3||action==7?10:2));
    lv_obj_set_y(character,base+bounce);
  } else if(fallback) {
    if(action!=0&&action!=4&&delta>1500) setAction(pet.sleeping?4:0);
    const bool blink=pet.sleeping||(millis()%3000<160);
    for(auto eye:eyes)lv_obj_set_height(eye,blink?4:18);
    lv_obj_set_y(fallback,105+int(std::sin(millis()/250.0)*(action?6:2)));
    frameCount++;
  }
}
static void updateLabels() {
  const int64_t utc=nowUtc();time_t epoch=utc;tm local{};
  if(utc)localtime_r(&epoch,&local);
  if(clockLabel) {
    char buf[40]="--:--";
    if(utc)snprintf(buf,sizeof(buf),"%02d:%02d",pet.hour24?local.tm_hour:((local.tm_hour+11)%12+1),local.tm_min);
    lv_label_set_text(clockLabel,buf);
    if(page==Clock)lv_obj_set_pos(clockLabel,80+int((millis()/60000)%5)*3,145+int((millis()/60000)%3)*3);
  }
  if(dateLabel) {
    char buf[100];const char *days[]={"日","一","二","三","四","五","六"};
    if(utc)snprintf(buf,sizeof(buf),"%04d/%02d/%02d  星期%s",local.tm_year+1900,local.tm_mon+1,local.tm_mday,days[local.tm_wday]);
    else snprintf(buf,sizeof(buf),"請設定時間");
    lv_label_set_text(dateLabel,buf);
  }
  if(page==Game) {
    char b[60];const int values[]={pet.hunger,pet.mood,pet.energy};const char *names[]={"飽足","心情","體力"};
    for(int i=0;i<3;i++)if(stats[i]){snprintf(b,sizeof(b),"%s %d",names[i],values[i]/100);lv_label_set_text(stats[i],b);}
  }
  if(infoLabel) {
    char b[600];
    if(page==Status) {
      char batteryText[32];battery::format(batteryReading,batteryText,sizeof(batteryText));
      snprintf(b,sizeof(b),"CHIMMY GOCHI 0.1\n親密 %lu  /  睡意 %ld\n亮度 %u  /  音量 %u\nSD %s  /  RTC %s\n觸控 %s  /  音效 %s\n%s  /  %s\n保存 %llu  /  %s\nPSRAM %u KB",
        (unsigned long)pet.bond,(long)pet.sleepy/100,pet.brightness,pet.volume,
        assets.ready?"OK":"ERROR",clockQuality?"OK":"--",touchOk?"OK":"ERROR",audioOk?"OK":"ERROR",
        batteryText,chargingDisabled?"充電停用":"充電未確認",(unsigned long long)pet.generation,saveLocked?"ERROR":dirty?"保存中":"已保存",ESP.getFreePsram()/1024);
    } else snprintf(b,sizeof(b),"%s%s  /  %s",utc?(local.tm_hour<12?"上午":"下午"):"",
      clockQuality==3?" NTP":clockQuality==2?" USB / 手動":clockQuality==1?" RTC":"請設定時間",
      pet.sleeping?"睡覺":"待機");
    lv_label_set_text(infoLabel,b);
  }
  if(page==SetTime) {
    const int v[]={timeEdit.tm_year+1900,timeEdit.tm_mon+1,timeEdit.tm_mday,timeEdit.tm_hour,timeEdit.tm_min};
    for(int i=0;i<5;i++) {char b[16];snprintf(b,sizeof(b),"%02d",v[i]);lv_label_set_text(timeFields[i],b);
      lv_obj_set_style_text_color(timeFields[i],color(i==editField?0xd15878:0x304057),0);}
  }
  if(toast&&elapsed(toastUntil))lv_obj_add_flag(toast,LV_OBJ_FLAG_HIDDEN);
}
static void powerTick() {
  static uint32_t last=0,lowAt=0;
  static bool buttonDown=false;
  const bool down=digitalRead(18)==LOW;
  if(down&&!buttonDown) {lastInput=millis();setDisplay(!displayOn,pet.brightness);}
  buttonDown=down;
  if(millis()-last<200)return;last=millis();
  if(pmuOk) {
    pmu.getIrqStatus();
    const bool shortPress=pmu.isPekeyShortPressIrq(),longPress=pmu.isPekeyLongPressIrq();pmu.clearIrqStatus();
    if(longPress) {
      stopSound();saveNow();display.setBrightness(0);display.displayOff();pmu.shutdown();
      notify("關機失敗：USB可能仍供電",10000);
    } else if(shortPress) {lastInput=millis();setDisplay(!displayOn,pet.brightness);}
    if(!pmu.isVbusIn()&&pmu.isBatteryConnect()&&pmu.getBattVoltage()<3400) {
      if(!lowAt)lowAt=millis();
      if(millis()-lowAt>10000) {stopSound();saveNow();pmu.shutdown();lowAt=millis();notify("低電量：準備關機");}
    } else lowAt=0;
  }
  if(transfer.active)return;
  if(displayOn) {
    const uint32_t idle=millis()-lastInput;
    if(pet.autoOff&&idle>120000) setDisplay(false,pet.brightness);
    else display.setBrightness(idle>30000?std::max<int>(20,pet.brightness/3):pet.brightness);
  }
}
static void networkTick() {
  if(!networkBusy)return;
  static bool requested=false;
  if(WiFi.status()==WL_CONNECTED&&!requested) {
    configTime(0,0,"pool.ntp.org","time.cloudflare.com");setenv("TZ","HKT-8",1);tzset();requested=true;
  }
  // SNTP sync status, rather than an already valid RTC time, determines success.
  extern bool networkSynced();
  if(requested&&networkSynced()) {
    clockQuality=3;const bool rtc=rtcWrite(nowUtc());changed();
    if(saveNow())notify(rtc?"時間已同步":"時間已同步，RTC寫入失敗");
    networkBusy=false;requested=false;WiFi.disconnect(false,false);WiFi.mode(WIFI_OFF);
  } else if(millis()-networkAt>15000) {
    notify("同步失敗：請用USB設定網絡",7000);networkBusy=false;requested=false;
    WiFi.disconnect(false,false);WiFi.mode(WIFI_OFF);
  }
}
#include <esp_sntp.h>
bool networkSynced(){return sntp_get_sync_status()==SNTP_SYNC_STATUS_COMPLETED;}

static void command(JsonDocument &request,JsonDocument &reply) {
  const char *cmd=request["cmd"]|"";
  String error;
  bool ok=false;
  if(strcmp(cmd,"status")==0) {
    ok=true;reply["version"]="0.1.0";reply["page"]=page;reply["sd"]=assets.ready;
    reply["psram"]=ESP.getPsramSize();reply["psram_free"]=ESP.getFreePsram();reply["heap"]=ESP.getFreeHeap();
    reply["touch"]=touchOk;reply["audio"]=audioOk;reply["pmic"]=pmuOk;reply["charging_disabled_verified"]=chargingDisabled;
    reply["battery_state"]=battery::stateName(batteryReading.state);
    if(batteryReading.state==battery::State::Ready)reply["battery_percent"]=batteryReading.percent;
    else reply["battery_percent"]=nullptr;
    reply["battery_text"]=batteryLabel?lv_label_get_text(batteryLabel):"";
    reply["battery_visible"]=batteryLabel&&!lv_obj_has_flag(batteryLabel,LV_OBJ_FLAG_HIDDEN);
    if(batteryLabel) {
      lv_obj_update_layout(batteryLabel);
      const auto font=lv_obj_get_style_text_font(batteryLabel,LV_PART_MAIN);
      const auto spacing=lv_obj_get_style_text_letter_space(batteryLabel,LV_PART_MAIN);
      const char *widest="電量 100%";
      reply["battery_text_width_px"]=lv_txt_get_width(lv_label_get_text(batteryLabel),
        strlen(lv_label_get_text(batteryLabel)),font,spacing,LV_TEXT_FLAG_NONE);
      reply["battery_max_width_px"]=lv_txt_get_width(widest,strlen(widest),font,spacing,LV_TEXT_FLAG_NONE);
      reply["battery_content_width_px"]=lv_obj_get_content_width(batteryLabel);
      reply["battery_clickable"]=lv_obj_has_flag(batteryLabel,LV_OBJ_FLAG_CLICKABLE);
    }
    reply["battery_sample_ms"]=batteryReadAt;
    reply["utc"]=nowUtc();int64_t rtc=0;reply["rtc_valid"]=rtcRead(rtc);reply["rtc_utc"]=rtc;
    reply["save_generation"]=pet.generation;reply["save_locked"]=saveLocked;reply["dirty"]=dirty;
    reply["outfit"]=pet.outfit;reply["hunger"]=pet.hunger;reply["mood"]=pet.mood;reply["energy"]=pet.energy;
    reply["sleeping"]=pet.sleeping;reply["bond"]=pet.bond;reply["flushes"]=flushCount;
    reply["action"]=action;reply["bubble_score"]=bubbleScore;reply["bubble_game"]=bubbleGame;
    reply["brightness"]=pet.brightness;reply["volume"]=pet.volume;reply["night_mute"]=pet.nightMute;
    reply["max_flush_us"]=maxFlushUs;reply["max_loop_us"]=maxLoopUs;reply["touches"]=touchCount;
    reply["frames"]=frameCount;reply["audio_failures"]=audioFailures.load();
    reply["pack"]=pack?pack->root:"";reply["asset_loading"]=assetLoading;reply["display_on"]=displayOn;
    reply["card_bytes"]=assets.ready?SD.cardSize():0;reply["uptime_ms"]=millis();
  } else if(strcmp(cmd,"capture_begin")==0) {
    if(screenPixels){capture=true;captureAt=millis();ok=true;}
    else error="diagnostic framebuffer unavailable";
  } else if(strcmp(cmd,"capture_row")==0) {
    const int row=request["row"]|-1;
    if(!capture||row<0||row>476)error="invalid capture row";
    else {
      String hex;hex.reserve(480*4*4);
      const auto p=reinterpret_cast<uint8_t *>(screenPixels+row*480);
      const char digits[]="0123456789abcdef";
      for(unsigned i=0;i<480*4*2;i++){hex+=digits[p[i]>>4];hex+=digits[p[i]&15];}
      reply["hex"]=hex;ok=true;captureAt=millis();
    }
  } else if(strcmp(cmd,"capture_end")==0) {capture=false;ok=true;}
  else if(strcmp(cmd,"time")==0) {
    if(request["utc"].is<int64_t>())ok=setTime(request["utc"].as<int64_t>());
    if(!ok)error="invalid time or save failed";
  } else if(strcmp(cmd,"save")==0) {ok=saveNow();if(!ok)error="save failed or locked";}
  else if(strcmp(cmd,"control")==0) {
    const int value=request["value"]|0;
    if(!request["value"].is<int>()||!((value>=GoGame&&value<=Tap)||(value>=Feed0&&value<Feed0+6)||
       (value>=Play0&&value<Play0+2)||(value>=Outfit0&&value<Outfit0+3)||(value>=BrightnessDown&&value<=ResetConfirm)))
      error="invalid control";
    else {lastInput=millis();control(value);ok=true;}
  } else if(strcmp(cmd,"wifi")==0) {
    const char *ssid=request["ssid"],*password=request["password"];
    if(!ssid||!*ssid||strlen(ssid)>32||!password||strlen(password)>63)error="invalid WiFi configuration";
    else {WiFi.persistent(true);WiFi.mode(WIFI_STA);WiFi.begin(ssid,password);
      networkBusy=true;networkAt=millis();ok=true;}
  } else if(strcmp(cmd,"reload")==0) {requestAssets();ok=assetLoading;if(!ok)error="cannot load pack";}
  else if(strcmp(cmd,"pack_begin")==0) {
    if(assetLoading)error="asset worker busy";
    else {ok=transfer.start(request["name"]|"",error);if(ok){stopSound();notify("USB素材傳輸中",60000);}}
  } else if(strcmp(cmd,"file_begin")==0)
    ok=transfer.open(request["name"]|"",request["size"]|0,request["sha256"]|"",error);
  else if(strcmp(cmd,"chunk")==0)
    ok=transfer.chunk(request["offset"]|SIZE_MAX,request["hex"]|"",error);
  else if(strcmp(cmd,"file_end")==0) ok=transfer.finish(error);
  else if(strcmp(cmd,"pack_end")==0) {ok=transfer.complete(error);if(ok)requestAssets();}
  else if(strcmp(cmd,"abort")==0) {transfer.abort();notify("傳輸已取消，舊素材保留");ok=true;}
  else if(strcmp(cmd,"screen")==0) {setDisplay(request["on"]|true,pet.brightness);lastInput=millis();ok=true;}
  else if(strcmp(cmd,"reboot")==0) {
    if(saveNow()){reply["ok"]=true;Serial.print('@');serializeJson(reply,Serial);Serial.println();Serial.flush();ESP.restart();}
    else error="save failed; reboot refused";
  } else error="unknown command";
  reply["ok"]=ok;
  if(!ok)reply["error"]=error.isEmpty()?"operation failed":error;
}
static void serialTick() {
  static char line[4096];static size_t used=0;static bool overflow=false;
  unsigned budget=4096;
  while(Serial.available()&&budget--) {
    char c=Serial.read();
    if(c=='\r')continue;
    if(c=='\n') {
      JsonDocument request,reply;
      if(overflow) {reply["ok"]=false;reply["error"]="line too long";}
      else if(deserializeJson(request,line,used)) {reply["ok"]=false;reply["error"]="invalid JSON";}
      else {reply["id"]=request["id"];command(request,reply);}
      Serial.print('@');serializeJson(reply,Serial);Serial.println();
      used=0;overflow=false;break;
    }
    if(used<sizeof(line)-1)line[used++]=c;else overflow=true;
  }
  if(transfer.active&&millis()-transfer.lastActivity>60000) {transfer.abort();notify("USB傳輸逾時，舊素材保留");}
}
void setup() {
  Serial.setRxBufferSize(8192);Serial.begin(115200);delay(2000);
  Serial.println("BOOT CHIMMY GOCHI 0.1.0");
  Serial.printf("PSRAM=%u heap=%u\n",ESP.getPsramSize(),ESP.getFreeHeap());
  setenv("TZ","HKT-8",1);tzset();
  if(!psramFound()||!boardBegin()) {
    while(true){Serial.println("FATAL board/PSRAM initialization");delay(2000);}
  }
  Serial.println("INIT LVGL");
  lv_init();
  lv_log_register_print_cb([](const char *msg){Serial.printf("LVGL %s",msg);});
  static lv_disp_draw_buf_t draw;
  auto b1=static_cast<lv_color_t *>(heap_caps_malloc(480*24*2,MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA));
  auto b2=static_cast<lv_color_t *>(heap_caps_malloc(480*24*2,MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA));
  if(!b1||!b2){while(true){Serial.println("FATAL LVGL buffers");delay(2000);}}
  lv_disp_draw_buf_init(&draw,b1,b2,480*24);
  static lv_disp_drv_t drv;lv_disp_drv_init(&drv);drv.hor_res=480;drv.ver_res=480;drv.draw_buf=&draw;
  drv.flush_cb=screenFlush;drv.rounder_cb=[](lv_disp_drv_t *,lv_area_t *a){a->x1&=~1;a->y1&=~1;a->x2|=1;a->y2|=1;};
  lv_disp_drv_register(&drv);
  static lv_indev_drv_t input;lv_indev_drv_init(&input);input.type=LV_INDEV_TYPE_POINTER;input.read_cb=readTouch;
  lv_indev_drv_register(&input);
  initBatteryIndicator();updateBattery();
  Serial.println("INIT save");
  saves.begin();const auto loaded=saves.load(pet);
  saveLocked=loaded==SaveStore::Load::Corrupt||loaded==SaveStore::Load::Unavailable;
  int64_t rtc=0;
  if(rtcRead(rtc)) {timeval tv{time_t(rtc),0};settimeofday(&tv,nullptr);clockQuality=1;}
  const uint32_t off=game::offline(pet,nowUtc());
  lastTickUs=esp_timer_get_time();lastInput=millis();
  Serial.println("INIT SD");
  assets.begin();showPage(int(pet.startMode));
  if(saveLocked)notify("資料損壞或保存初始化失敗：請檢查設定",15000);
  else {
    saveNow();
    if(loaded==SaveStore::Load::Recovered)notify("已復原上一份保存資料",10000);
    else if(off)notify("離線期間已按上限計算",6000);
  }
  requestAssets();
  Serial.printf("READY display=1 touch=%d audio=%d pmu=%d SD=%d psram=%u RTC=%d saveLocked=%d\n",
    touchOk,audioOk,pmuOk,assets.ready,ESP.getPsramSize(),clockQuality,saveLocked);
}
void loop() {
  const uint64_t start=esp_timer_get_time();
  advanceNow();updateBattery();serialTick();powerTick();networkTick();
  AssetResult loaded;
  if(assets.poll(loaded)) {
    assetLoading=false;
    if(loaded.generation!=sceneGeneration)delete loaded.pack;
    else if(!loaded.pack) {
      if(pack&&pet.outfit!=pack->outfit){pet.outfit=pack->outfit;changed();}
      showPage(page);notify(loaded.error,10000);
    }
    else {
      stopSound();
      lv_img_cache_invalidate_src(nullptr);
      auto oldPack=std::move(pack);
      pack.reset(loaded.pack);showPage(page);
      notify(pack->placeholder?"示範素材已載入":"素材已載入");
    }
  }
  if(!capture&&millis()-lastFrameAt>=66) {lastFrameAt=millis();updateAnimation();}
  static uint32_t lastLabels=0;
  if(millis()-lastLabels>=250){lastLabels=millis();updateLabels();}
  if(dirty&&!saveLocked&&!transfer.active&&millis()-dirtyAt>=2000&&
     (urgentSave||millis()-lastSave>=60000||pet.generation<2)) saveNow();
  if(bubbleGame&&millis()-bubbleAt>30000){showPage(Game);notify("再玩一次");}
  if(page==Game&&millis()-lastInput>45000&&!transfer.active)showPage(Ambient);
  if(capture&&millis()-captureAt>15000)capture=false;
  if(!capture)lv_timer_handler();
  if(audioFailures.load()!=audioFailureSeen) {
    audioFailureSeen=audioFailures.load();notify("音效播放失敗");
  }
  maxLoopUs=std::max<uint32_t>(maxLoopUs,esp_timer_get_time()-start);
  delay(3);
}
