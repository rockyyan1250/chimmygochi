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
#include "icons.h"
#include "Kmb.h"
#include "Net.h"
#include "Bus.h"
#include "Games.h"
#include <WebServer.h>

LV_FONT_DECLARE(font_tc);
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
static lv_obj_t *bondLabel=nullptr,*clockLabel=nullptr,*dateLabel=nullptr,*infoLabel=nullptr,*stats[3]{},*settingsPanel=nullptr;
// Bus screen: per line the stop, direction and next-bus labels.
static lv_obj_t *busStop[bus::kLines]{},*busDest[bus::kLines]{},*busEta[bus::kLines]{};
static uint32_t busRevision=0,busFetchAt=0,busMapAt=0;
static int busBoard[bus::kLines]={-1,-1};  // line -> board index in the last batch
static lv_obj_t *sheet=nullptr,*foodSprite=nullptr;  // care menu over the room; snack being eaten
// Viewing mode (room): every control hidden, CHIMMY larger; any touch brings the room back.
constexpr int kViewZoom=300;
static bool viewing=false;
static lv_obj_t *viewPad=nullptr,*viewHidden[48]{};
static unsigned viewHiddenCount=0;
static int queuedAction=-1;  // plays when the current one-shot clip ends (e.g. joy after a favourite)
static lv_obj_t *wifiIcon=nullptr,*batteryBox=nullptr,*batteryLabel=nullptr,*batteryFill=nullptr,*powerIcon=nullptr;
static battery::Reading batteryReading{battery::State::Unavailable,-1};
static bool batterySampled=false,usbPower=false,charging=false;
static lv_img_dsc_t thumbImages[kMaxBackgrounds]{},outfitImages[kMaxOutfits]{};
static bool dressAfterLoad=false;  // play the dress-up clip once the new outfit has loaded
static uint32_t batteryReadAt=0;
static lv_obj_t *fallback=nullptr,*eyes[2]{},*fallbackBody=nullptr;
static lv_img_dsc_t atlasImage{},bgImage{};
static uint32_t lastFrame=UINT32_MAX,lastFrameAt=0;
static char toastMessage[200]{};
static tm timeEdit{};
static lv_obj_t *timeFields[5]{};
static int editField=0;
static games::Id playing=games::Catch;  // the mini game on the Play page
static uint32_t audioFailureSeen=0;
static bool urgentSave=false,capture=false;
static uint32_t captureAt=0;
// Actions 0..7 are the pack's named clips; these index its optional clips.
constexpr int ActTap0=8,ActWalk=ActTap0+int(kMaxTaps),ActSit=ActWalk+1,ActGesture0=ActSit+1;
// Standby screen: the pet rests, walks to a random spot, sometimes sits, and repeats.
// The pet's own life (Behavior.h): the step being played and where a walk is heading.
static behavior::Engine engine(1);
static behavior::Step step;
static bool stepInterrupted=false;
static float wanderX=0,wanderY=0,targetX=0,targetY=0;
static uint32_t stepAt=0,lastWanderMs=0;
static int lastTap=-1;

// Page 2 and control 3 belonged to the removed clock mode; the numbers stay for the USB test tools.
enum Page { Game=0,Ambient=1,Clock=2,Settings=3,Foods=4,Toys=5,Rooms=6,Status=7,SetTime=8,WifiSetup=9,Bus=10,BusSetup=11,GamesHub=12,Play=13,Outfits=14 };
enum Control { GoGame=1,GoAmbient,GoClock,GoSettings,GoFoods,GoToys,GoRooms,Sleep,
  GoStatus,Back,Tap,Feed0=20,Play0=30,Room0=40,BrightnessDown=50,BrightnessUp,VolumeDown,VolumeUp,
  AutoOff,Hour24,NightMute,StartMode,OfflineCap,EditTime,TimePrev,TimeNext,TimeMinus,TimePlus,TimeSave,
  Reload,Sync,ResetConfirm=68,ResetYes,GoBus=70,GoBusSetup,ToggleSheet,GoGames,AutoDim,ViewMode,GoOutfits,PlayGame0=80,Outfit0=90 };
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
static lv_obj_t *box(lv_obj_t *parent,int x,int y,int w,int h,uint32_t bg,int radius) {
  auto o=lv_obj_create(parent);lv_obj_set_pos(o,x,y);lv_obj_set_size(o,w,h);
  lv_obj_set_style_bg_color(o,color(bg),0);lv_obj_set_style_radius(o,radius,0);
  lv_obj_set_style_border_width(o,0,0);lv_obj_set_style_pad_all(o,0,0);
  lv_obj_clear_flag(o,LV_OBJ_FLAG_SCROLLABLE);lv_obj_clear_flag(o,LV_OBJ_FLAG_CLICKABLE);
  return o;
}
// Pill at the top centre (away from the panel's rounded corners): power mark, battery, percent.
static void initBatteryIndicator() {
  wifiIcon=lv_img_create(lv_layer_top());lv_obj_set_pos(wifiIcon,128,24);lv_img_set_src(wifiIcon,&icon_wifi_off);
  batteryBox=box(lv_layer_top(),164,16,152,44,0xfffaf3,LV_RADIUS_CIRCLE);
  lv_obj_set_style_bg_opa(batteryBox,LV_OPA_90,0);
  powerIcon=lv_img_create(batteryBox);lv_obj_set_pos(powerIcon,10,11);lv_obj_add_flag(powerIcon,LV_OBJ_FLAG_HIDDEN);
  auto shell=box(batteryBox,36,13,36,18,0xffffff,4);
  lv_obj_set_style_border_width(shell,2,0);lv_obj_set_style_border_color(shell,color(0x4a3f4f),0);
  batteryFill=box(shell,1,1,0,12,0x5cc27a,2);
  box(batteryBox,72,18,4,8,0x4a3f4f,1);
  batteryLabel=label(batteryBox,"--",82,9,64);
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
    const int level=next.state==battery::State::Ready?next.percent:0;
    lv_obj_set_width(batteryFill,std::max(level?2:0,30*level/100));
    lv_obj_set_style_bg_color(batteryFill,color(level<=20?0xff6b6b:0x5cc27a),0);
  }
  // Plug: USB power only (full, paused for heat, or charge limits unverified); bolt: charging.
  chargeGuard();
  const net::State ws=net::state();
  lv_img_set_src(wifiIcon,ws==net::State::Connected?&icon_wifi_on:ws==net::State::Connecting?&icon_wifi_wait:&icon_wifi_off);
  const bool usb=pmuOk&&pmu.isVbusIn(),charge=pmuOk&&pmu.isCharging();
  if(!batterySampled||usb!=usbPower||charge!=charging) {
    usbPower=usb;charging=charge;
    if(charge||usb) {lv_img_set_src(powerIcon,charge?&icon_charge:&icon_usb);lv_obj_clear_flag(powerIcon,LV_OBJ_FLAG_HIDDEN);}
    else lv_obj_add_flag(powerIcon,LV_OBJ_FLAG_HIDDEN);
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
static lv_obj_t *iconButton(lv_obj_t *parent,const lv_img_dsc_t *icon,int x,int y,int w,int h,int id,uint32_t bg=0xffffff) {
  auto b=lv_btn_create(parent);lv_obj_set_pos(b,x,y);lv_obj_set_size(b,w,h);
  lv_obj_set_style_radius(b,LV_RADIUS_CIRCLE,0);lv_obj_set_style_bg_color(b,color(bg),0);lv_obj_set_style_bg_opa(b,LV_OPA_90,0);
  lv_obj_set_style_shadow_width(b,0,0);lv_obj_set_style_border_width(b,0,0);lv_obj_set_style_pad_all(b,0,0);
  lv_obj_add_event_cb(b,event,LV_EVENT_CLICKED,reinterpret_cast<void *>(static_cast<intptr_t>(id)));
  auto img=lv_img_create(b);lv_img_set_src(img,icon);lv_obj_center(img);
  return b;
}
static void notify(const char *message,uint32_t duration) {
  snprintf(toastMessage,sizeof(toastMessage),"%s",message);toastUntil=millis()+duration;
  Serial.printf("NOTICE %s\n",message);
  if(toast) {lv_label_set_text(toast,toastMessage);lv_obj_clear_flag(toast,LV_OBJ_FLAG_HIDDEN);}
}
static bool elapsed(uint32_t deadline) {return int32_t(millis()-deadline)>=0;}
static void requestAssets() {
  if(assetLoading||transfer.active) return;
  const String root=assets.activeRoot();
  if(!assets.ready||root.isEmpty()) {notify("示範素材：使用內建圖形，請由USB安裝素材",8000);return;}
  assetLoading=assets.request(root.c_str(),pet.background,pet.outfit,++sceneGeneration,pack.get());
  if(!assetLoading) notify("素材載入失敗");
}
static bool muteNow() {
  if(!pet.volume||!displayOn) return true;
  if(!pet.nightMute) return false;
  int64_t t=nowUtc();if(!t) return true;
  time_t epoch=t;tm local{};localtime_r(&epoch,&local);
  return local.tm_hour>=22||local.tm_hour<8;
}
static const Clip &clipFor(int a) {
  if(a>=ActTap0&&a<ActWalk) return pack->taps[a-ActTap0];
  if(a>=ActGesture0&&a<ActGesture0+int(behavior::kGestures)) return pack->gestures[a-ActGesture0];
  return a==ActWalk?pack->walk:a==ActSit?pack->sit:pack->clips[a];
}
// Idle, sleep, walk and sit are resting states: new actions may interrupt them.
// Idle, sleep and the pet's own acts (walk, sit, gestures): a touch or care act may cut in.
static bool resting(int a) {return a==0||a==4||a==ActWalk||a==ActSit||(a>=ActGesture0&&a<ActGesture0+int(behavior::kGestures));}
static void soundFor(int next) {
  if(muteNow()||!pack) return;
  const int sound=clipFor(next).sound;
  if(sound>=0 && !playSound(pack->sounds[sound].data,pack->sounds[sound].size,pet.volume))
    notify("音效播放失敗");
}
static void setAction(int next) {
  stopSound();action=next;actionAt=millis();lastFrame=UINT32_MAX;
  soundFor(next);
}
static void drawFrame(uint32_t frame) {
  if(!pack||!character||frame==lastFrame) return;
  atlasImage={};atlasImage.header.cf=LV_IMG_CF_TRUE_COLOR_ALPHA;
  atlasImage.header.w=pack->atlasHeader.width;atlasImage.header.h=pack->atlasHeader.height;
  atlasImage.data_size=pack->atlasHeader.width*pack->atlasHeader.height*3;
  atlasImage.data=pack->atlas->data+16+frame*atlasImage.data_size;
  lv_img_cache_invalidate_src(&atlasImage);lv_img_set_src(character,&atlasImage);
  lastFrame=frame;frameCount++;
}
// Character placement: feet near y=352 on the game page; the standby screen moves it within
// a floor band whose lowest feet line (y~380) stays above the clock.
static int charSize() {return pack?pack->atlasHeader.width:256;}
static int gameY() {return 356-charSize();}
static int charZoom=154;  // 256 = 100%; bus page CHIMMY, sized to the floor left under the cards
static int zoomTop(int feetY) {const int h=charSize()/2;return feetY-h-(charSize()-4-h)*charZoom/256;}
static int restX() {return (480-charSize())/2;}
static int restY() {return 352-charSize()+16;}
static int homeY() {return viewing?zoomTop(440):page==Game?gameY():page==Bus?zoomTop(468):restY();}
// Floor band per page: on the game page the feet stay above the stat chips (y~356).
static int wanderMinY() {return viewing?zoomTop(410):page==Game?gameY()-28:page==Bus?zoomTop(460):std::max(restY()-40,62);}  // head below the cards / status strip
static int wanderMaxY() {return viewing?zoomTop(452):page==Game?gameY():page==Bus?zoomTop(468):restY()-8;}  // standby: feet above the date
static int localHour() {
  const int64_t t=nowUtc();if(!t) return -1;
  time_t e=t;tm l{};localtime_r(&e,&l);return l.tm_hour;
}
static void resetWander() {
  step=behavior::Step{};step.ms=2500;stepAt=lastWanderMs=millis();stepInterrupted=false;
  wanderX=targetX=restX();wanderY=targetY=homeY();
}
static void sleepNow(bool own) {
  advanceNow();pet.sleeping=true;pet.autoSlept=own;pet.sleepSeconds=0;changed();
  if(page==Game||page==Ambient||page==Bus) showPage(page);  // moon/sun button and sleeping pose
}
static void wakeNow() {
  advanceNow();pet.sleeping=false;pet.autoSlept=false;pet.sleepSeconds=0;changed();
  engine.woke(millis());
  if(page==Game||page==Ambient||page==Bus) showPage(page);
}
// Night sleep must not depend on the animation (it stops while the screen is off):
// checked every second from loop(). With the screen on, the engine dozes first instead.
static void sleepTick() {
  const int hour=localHour();if(hour<0) return;
  if(pet.sleeping) {if(pet.autoSlept&&!behavior::night(hour)) wakeNow();return;}
  const bool engineSees=displayOn&&(page==Game||page==Ambient||page==Bus)&&pack;
  if(!engineSees&&behavior::night(hour)&&millis()-lastInput>=behavior::kSleepIdleMs) sleepNow(true);
}
static void startStep(uint32_t now) {
  using behavior::Kind;
  switch(step.kind) {
    case Kind::Rest: if(action!=0) setAction(0);break;
    case Kind::Sit: setAction(pack->sit.count?ActSit:0);break;
    case Kind::Gesture: setAction(ActGesture0+step.gesture);break;
    case Kind::Sleep: sleepNow(true);break;
    case Kind::Walk: {
    // The frame's transparent margin may leave the screen; the body stays inside. Each walk
    // covers at least 120px, turning back when the far side is too close.
    // Bus page: the smaller body may go further out but stays clear of the rounded corners.
    const int zoomNow=page==Bus||viewing?charZoom:256,body=110*zoomNow/256,half=charSize()/2;
    const int minX=20+body-half,maxX=460-body-half,minY=wanderMinY(),maxY=wanderMaxY();
    // int() keeps the negative minX out of unsigned arithmetic with esp_random().
    auto random=[](int n){return int(esp_random()%unsigned(n));};
    targetX=minX+random(maxX-minX+1);
    if(std::fabs(targetX-wanderX)<120) {
      const bool right=wanderX+120<=maxX&&(wanderX-120<minX||random(2));
      targetX=right?std::min<float>(maxX,wanderX+120+random(60)):std::max<float>(minX,wanderX-120-random(60));
    }
    targetX=std::max<float>(minX,std::min<float>(maxX,targetX));
    targetY=minY+random(maxY-minY+1);
    setAction(pack->walk.count?ActWalk:0);
    break;
    }
  }
  stepAt=now;
}
// Plays the current step and asks the engine for the next one when it ends.
static void wanderTick() {
  using behavior::Kind;
  const uint32_t now=millis();
  const float dt=std::min<uint32_t>(now-lastWanderMs,200)/1000.0f;lastWanderMs=now;
  if(pet.sleeping) return;
  if(!resting(action)) {stepInterrupted=true;return;}  // a touch or care act is playing
  if(sheet) {if(action!=0) setAction(0);stepInterrupted=true;return;}  // menu open: stand and wait
  bool done=stepInterrupted;
  if(!done) switch(step.kind) {
    case Kind::Walk: {
      const float dx=targetX-wanderX,dy=targetY-wanderY,dist=std::sqrt(dx*dx+dy*dy),move=28*dt;
      if(dist>move) {wanderX+=dx/dist*move;wanderY+=dy/dist*move;}
      else {wanderX=targetX;wanderY=targetY;done=true;}
      break;
    }
    // A one-shot clip hands back to idle (action 0) when it ends; looping ones (doze) run for step.ms.
    case Kind::Gesture: done=action==0||(step.ms&&now-stepAt>=step.ms);break;
    default: done=now-stepAt>=step.ms;break;
  }
  if(!done) return;
  if(stepInterrupted) {engine.interrupt();stepInterrupted=false;}
  behavior::Context c;
  c.now=now;c.hour=localHour();c.idleMs=now-lastInput;c.bond=pet.bond;c.calm=page==Bus;
  c.hasSit=pack->sit.count>0;c.available=pack->gestureMask;c.asleep=pet.sleeping;
  step=engine.next(c);
  startStep(now);
}
static void makeScene() {
  if(pack) {
    bgImage={};bgImage.header.cf=LV_IMG_CF_TRUE_COLOR;bgImage.header.w=480;bgImage.header.h=480;
    bgImage.data=pack->background.data+16;bgImage.data_size=480*480*2;
    background=lv_img_create(screen);lv_img_set_src(background,&bgImage);lv_obj_set_pos(background,0,0);
    character=lv_img_create(screen);lv_obj_set_pos(character,restX(),page==Game?gameY():restY());
    lv_obj_add_flag(character,LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(character,event,LV_EVENT_CLICKED,reinterpret_cast<void *>(static_cast<intptr_t>(Tap)));
  } else {
    fallback=lv_obj_create(screen);lv_obj_set_size(fallback,190,190);lv_obj_set_pos(fallback,145,105);
    lv_obj_clear_flag(fallback,LV_OBJ_FLAG_SCROLLABLE);lv_obj_set_style_radius(fallback,75,0);
    const uint32_t palette[]={0x6bd8d0,0xada4ed,0xf5a8bf};
    lv_obj_set_style_bg_color(fallback,color(palette[pet.background]),0);lv_obj_set_style_border_width(fallback,4,0);
    lv_obj_set_style_border_color(fallback,color(0xffffff),0);
    for(unsigned i=0;i<2;i++) {
      eyes[i]=lv_obj_create(fallback);lv_obj_set_size(eyes[i],12,18);lv_obj_set_pos(eyes[i],45+i*65,60);
      lv_obj_set_style_bg_color(eyes[i],color(0x304057),0);lv_obj_set_style_radius(eyes[i],8,0);lv_obj_set_style_border_width(eyes[i],0,0);
    }
    fallbackBody=label(fallback,"~",66,88,40);lv_obj_set_style_text_font(fallbackBody,&lv_font_montserrat_48,0);
    lv_obj_add_event_cb(fallback,event,LV_EVENT_CLICKED,reinterpret_cast<void *>(static_cast<intptr_t>(Tap)));
  }
}
// Layout keeps controls clear of the panel's rounded corners (radius ~48px): the top row
// spans x=70..410 and the bottom row x=74..406, leaving the middle for the character.
// A soft pill behind the clock keeps it readable on dark rooms (night, sky).
static void clockBackdrop(lv_obj_t *l) {
  lv_obj_set_style_bg_color(l,color(0xfffaf3),0);lv_obj_set_style_bg_opa(l,LV_OPA_80,0);
  lv_obj_set_style_radius(l,LV_RADIUS_CIRCLE,0);lv_obj_set_style_pad_ver(l,2,0);
}
static void topRow(bool home) {
  if(home) {
    iconButton(screen,pet.sleeping?&icon_wake:&icon_sleep,70,12,48,48,Sleep,0xede7fb);
    iconButton(screen,&icon_nav_settings,400,12,48,48,GoSettings,0xf3f0ea);
  } else iconButton(screen,&icon_back,70,12,48,48,Back,0xede7fb);
}
// Settings row: icon, a short Cantonese name, then the control on the right (from x=236).
static void settingRow(lv_obj_t *panel,int y,const lv_img_dsc_t *icon,const char *name) {
  auto img=lv_img_create(panel);lv_img_set_src(img,icon);lv_obj_set_pos(img,18,y+15);
  label(panel,name,62,y+14,170);
}
static void switchRow(lv_obj_t *panel,int y,const lv_img_dsc_t *icon,const char *name,bool on,int id) {
  settingRow(panel,y,icon,name);
  auto sw=lv_switch_create(panel);lv_obj_set_pos(sw,316,y+14);lv_obj_set_size(sw,84,38);
  if(on)lv_obj_add_state(sw,LV_STATE_CHECKED);
  lv_obj_set_style_bg_color(sw,color(0x7fd3b8),LV_PART_INDICATOR|LV_STATE_CHECKED);
  lv_obj_add_event_cb(sw,event,LV_EVENT_CLICKED,reinterpret_cast<void *>(static_cast<intptr_t>(id)));
}
static void gameSound(int slot) {
  if(muteNow()||!pack||slot<0||slot>3) return;
  playSound(pack->sounds[slot].data,pack->sounds[slot].size,pet.volume);
}
// Playing together: up to +10 affection and a lift in mood per finished game.
static void gameReward(unsigned score) {
  advanceNow();const unsigned n=std::min(score,10u);
  pet.bond=std::min<uint32_t>(1000000,pet.bond+n);pet.mood=game::bounded(pet.mood+int(n)*150);changed();
}
static void showPage(int next) {
  if(next==Game) next=Ambient;
  if(page==Play) games::stop();  // its objects go with the old screen  // v0.3: no gauge page; the room is home and care opens over it
  if(page==WifiSetup&&next!=WifiSetup) net::stopPortal();  // the setup AP lives only on its page
  if(viewing) {viewing=false;viewPad=nullptr;viewHiddenCount=0;lv_obj_clear_flag(lv_layer_top(),LV_OBJ_FLAG_HIDDEN);}
  const lv_coord_t keepScroll=page==Settings&&next==Settings&&settingsPanel?lv_obj_get_scroll_y(settingsPanel):0;
  auto oldScreen=screen;
  screen=lv_obj_create(nullptr);
  lv_obj_clear_flag(screen,LV_OBJ_FLAG_SCROLLABLE);lv_obj_set_style_bg_color(screen,color(0xf3efe8),0);
  lv_scr_load(screen);
  if(oldScreen)lv_obj_del(oldScreen);
  page=next;character=background=fallback=fallbackBody=bondLabel=clockLabel=dateLabel=infoLabel=settingsPanel=nullptr;
  for(unsigned i=0;i<bus::kLines;i++) busStop[i]=busDest[i]=busEta[i]=nullptr;
  toast=sheet=foodSprite=nullptr;queuedAction=-1;for(auto &p:stats)p=nullptr;
  stopSound();action=pet.sleeping?4:0;actionAt=millis();lastFrame=UINT32_MAX;
  if(page==Game||page==Ambient) {
    // Animation pauses while the screen is off; show the resting frame so waking never finds an empty image.
    resetWander();
    makeScene();if(pack)drawFrame(clipFor(action).frames[0]);
    topRow(true);
    iconButton(screen,&icon_view,344,12,48,48,ViewMode,0xf3f0ea);
    {
      auto strip=box(screen,128,66,224,32,0xfffaf3,LV_RADIUS_CIRCLE);lv_obj_set_style_bg_opa(strip,LV_OPA_80,0);
      const lv_img_dsc_t *icons[]={&icon_stat_food,&icon_stat_mood,&icon_stat_energy};
      const uint32_t tint[]={0xffb347,0xff7a93,0xffd23f};
      for(int i=0;i<3;i++) {
        auto img=lv_img_create(strip);lv_img_set_src(img,icons[i]);lv_obj_set_pos(img,8+i*54,6);
        stats[i]=lv_bar_create(strip);lv_obj_set_pos(stats[i],30+i*54,12);lv_obj_set_size(stats[i],24,8);
        lv_bar_set_range(stats[i],0,100);lv_obj_clear_flag(stats[i],LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(stats[i],color(0xe9e2d6),LV_PART_MAIN);
        lv_obj_set_style_bg_color(stats[i],color(tint[i]),LV_PART_INDICATOR);
      }
      auto paw=lv_img_create(strip);lv_img_set_src(paw,&icon_stat_bond);lv_obj_set_pos(paw,170,6);
      bondLabel=label(strip,"0",192,0,32,0x594761);
    }
    {
      dateLabel=label(screen,"",120,360,240,0x594761);clockBackdrop(dateLabel);
      lv_obj_set_style_text_align(dateLabel,LV_TEXT_ALIGN_CENTER,0);
      clockLabel=label(screen,"--:--",150,400,180);clockBackdrop(clockLabel);
      lv_obj_update_layout(clockLabel);
      const int rowY=lv_obj_get_y(clockLabel)+lv_obj_get_height(clockLabel)/2-26;
      lv_obj_set_style_text_font(clockLabel,&lv_font_montserrat_48,0);
      lv_obj_set_style_text_align(clockLabel,LV_TEXT_ALIGN_CENTER,0);
      iconButton(screen,&icon_nav_bus,70,rowY,64,52,GoBus,0xf7f3ed);
      iconButton(screen,&icon_nav_heart,346,rowY,64,52,ToggleSheet,0xffe3ea);
    }
  } else if(page==Bus) {
    // Next buses for the configured lines (or one hint card), the time and date, and CHIMMY
    // wandering the floor below, as large as the space under the cards allows.
    unsigned shown=0;
    for(unsigned i=0;i<bus::kLines;i++) shown+=bus::used(i);
    const unsigned cards=shown?shown:1;
    const int cardsBottom=146+int(cards)*92-4;
    // Standing height ~0.855 of the frame; head clears the cards with feet at y=460.
    charZoom=std::min(256,(460-cardsBottom-6)*256*100/(charSize()*855/10));
    resetWander();
    makeScene();
    if(character) {
      if(charZoom<256) lv_img_set_zoom(character,charZoom);
      lv_obj_set_pos(character,restX(),homeY());
      drawFrame(clipFor(action).frames[0]);
    }
    iconButton(screen,&icon_back,70,12,48,48,Back,0xede7fb);
    iconButton(screen,&icon_nav_settings,400,12,48,48,GoBusSetup,0xf3f0ea);
    auto pill=box(screen,140,62,200,80,0xfffaf3,28);lv_obj_set_style_bg_opa(pill,LV_OPA_80,0);
    clockLabel=label(pill,"--:--",0,-4,200);
    lv_obj_set_style_text_font(clockLabel,&lv_font_montserrat_48,0);lv_obj_set_style_text_align(clockLabel,LV_TEXT_ALIGN_CENTER,0);
    dateLabel=label(pill,"",0,46,200,0x594761);lv_obj_set_style_text_align(dateLabel,LV_TEXT_ALIGN_CENTER,0);
    for(unsigned i=0,slot=0;i<bus::kLines;i++) {
      if(!bus::used(i)&&(shown||i)) continue;  // unused slots hidden; one hint card when none is set
      auto card=box(screen,40,146+int(slot++)*92,400,88,0xfffaf3,20);lv_obj_set_style_bg_opa(card,LV_OPA_90,0);
      const auto &l=bus::line(i);
      auto badge=box(card,10,15,72,58,0xe85a5a,16);
      auto num=lv_label_create(badge);lv_label_set_text(num,bus::used(i)?l.route:"-");
      lv_obj_set_style_text_font(num,&lv_font_montserrat_24,0);lv_obj_set_style_text_color(num,color(0xffffff),0);lv_obj_center(num);
      busStop[i]=label(card,bus::used(i)?l.stopName:"未設定路線",92,-1,300);
      busDest[i]=label(card,"",92,27,300,0x7a6e7f);
      busEta[i]=label(card,"",92,55,300,0xd15878);
      for(auto t:{busStop[i],busDest[i],busEta[i]}) lv_label_set_long_mode(t,LV_LABEL_LONG_DOT);
      if(bus::used(i)) {char b[80];snprintf(b,sizeof(b),"往 %s",l.dest);lv_label_set_text(busDest[i],b);}
      else lv_label_set_text(busDest[i],"撳右上角設定");
    }
    busRevision=0;  // fetch at once
  } else if(page==GamesHub) {
    // Four mini games as cards, each with its best score.
    iconButton(screen,&icon_back,70,12,48,48,GoAmbient,0xede7fb);
    auto title=label(screen,"小遊戲",90,70,300);lv_obj_set_style_text_align(title,LV_TEXT_ALIGN_CENTER,0);
    const lv_img_dsc_t *icons[]={&icon_g_card0,&icon_g_card1,&icon_g_card2,&icon_g_card3};
    const uint32_t tints[]={0xffe8cc,0xc0e5e2,0xffd6e0,0xfff1b8};
    for(unsigned i=0;i<games::kGames;i++) {
      auto b=lv_btn_create(screen);lv_obj_set_pos(b,54+(i%2)*192,112+(i/2)*172);lv_obj_set_size(b,180,160);
      lv_obj_set_style_radius(b,28,0);lv_obj_set_style_bg_color(b,color(tints[i]),0);
      lv_obj_set_style_shadow_width(b,0,0);lv_obj_set_style_pad_all(b,0,0);
      lv_obj_add_event_cb(b,event,LV_EVENT_CLICKED,reinterpret_cast<void *>(static_cast<intptr_t>(PlayGame0+i)));
      auto img=lv_img_create(b);lv_img_set_src(img,icons[i]);lv_obj_align(img,LV_ALIGN_TOP_MID,0,14);
      auto name=label(b,games::names[i],0,84,180);lv_obj_set_style_text_align(name,LV_TEXT_ALIGN_CENTER,0);
      char best[24];snprintf(best,sizeof(best),"最高 %u",games::best(games::Id(i)));
      auto bl=label(b,best,0,116,180,0x7a6e7f);lv_obj_set_style_text_align(bl,LV_TEXT_ALIGN_CENTER,0);
    }
  } else if(page==Outfits) {
    // Wardrobe: one card per outfit with its picture; the worn one has a pink frame.
    iconButton(screen,&icon_back,70,12,48,48,GoAmbient,0xede7fb);
    auto title=label(screen,"換衫",90,70,300);lv_obj_set_style_text_align(title,LV_TEXT_ALIGN_CENTER,0);
    const unsigned count=pack?pack->outfitCount:0;
    if(!count) {
      infoLabel=label(screen,"示範素材：請先由USB安裝素材",60,220,360);
      lv_obj_set_style_text_align(infoLabel,LV_TEXT_ALIGN_CENTER,0);
    }
    auto panel=lv_obj_create(screen);lv_obj_set_pos(panel,40,112);lv_obj_set_size(panel,400,354);
    lv_obj_set_style_pad_all(panel,0,0);lv_obj_set_style_border_width(panel,0,0);lv_obj_set_style_bg_opa(panel,LV_OPA_TRANSP,0);
    lv_obj_set_scrollbar_mode(panel,LV_SCROLLBAR_MODE_OFF);
    for(unsigned i=0;i<count;i++) {
      const bool worn=i==pack->outfit;
      auto b=lv_btn_create(panel);lv_obj_set_pos(b,(i%2)*208,(i/2)*172);lv_obj_set_size(b,192,160);
      lv_obj_set_style_radius(b,28,0);lv_obj_set_style_bg_color(b,color(worn?0xffe3ea:0xfff1d6),0);
      lv_obj_set_style_shadow_width(b,0,0);lv_obj_set_style_pad_all(b,0,0);
      lv_obj_set_style_border_width(b,worn?4:0,0);lv_obj_set_style_border_color(b,color(0xff8fab),0);
      lv_obj_add_event_cb(b,event,LV_EVENT_CLICKED,reinterpret_cast<void *>(static_cast<intptr_t>(Outfit0+i)));
      if(pack->outfitThumbs[i].data) {
        auto &t=outfitImages[i];t={};t.header.cf=LV_IMG_CF_TRUE_COLOR_ALPHA;
        t.header.w=pack->outfitThumbHeaders[i].width;t.header.h=pack->outfitThumbHeaders[i].height;
        t.data_size=t.header.w*t.header.h*3;t.data=pack->outfitThumbs[i].data+16;
        auto img=lv_img_create(b);lv_img_set_src(img,&t);lv_obj_align(img,LV_ALIGN_TOP_MID,0,10);
      }
      auto name=label(b,pack->outfitNames[i],0,0,180);lv_obj_set_style_text_align(name,LV_TEXT_ALIGN_CENTER,0);
      lv_obj_align(name,LV_ALIGN_BOTTOM_MID,0,-12);  // same spot with or without the worn frame
    }
    if(count==1) {  // only the everyday clothes so far
      auto soon=box(panel,208,0,192,160,0xf3efe8,28);
      auto l=label(soon,"新衫\n即將登場",0,50,192,0x9a8f9f);lv_obj_set_style_text_align(l,LV_TEXT_ALIGN_CENTER,0);
    }
  } else if(page==Play) {
    games::Host h;
    h.screen=screen;h.pack=pack.get();h.font=&font_tc;h.sound=gameSound;h.reward=gameReward;
    h.restart=[]{showPage(Play);};h.exit=[]{showPage(GamesHub);};
    games::start(playing,h);
  } else if(page==BusSetup) {
    iconButton(screen,&icon_back,70,12,48,48,Back,0xede7fb);
    auto title=label(screen,"巴士路線設定",90,72,300);lv_obj_set_style_text_align(title,LV_TEXT_ALIGN_CENTER,0);
    const String ip=net::ip();
    if(ip.length()) {
      // The PIN rides in the URL, so only someone who can see this screen can change the lines.
      char url[64];snprintf(url,sizeof(url),"http://%s/bus?pin=%s",ip.c_str(),bus::pin());
      auto qr=lv_qrcode_create(screen,196,color(0x2b2530),color(0xffffff));
      lv_obj_set_pos(qr,142,112);lv_obj_set_style_border_color(qr,color(0xffffff),0);lv_obj_set_style_border_width(qr,8,0);
      lv_qrcode_update(qr,url,strlen(url));
      char b[120];snprintf(b,sizeof(b),"用手機掃描 QR Code\n（手機要連同一個 Wi-Fi）\nPIN %s",bus::pin());
      infoLabel=label(screen,b,60,326,360);
    } else infoLabel=label(screen,"請先喺設定度連接 Wi-Fi",60,200,360);
    lv_obj_set_style_text_align(infoLabel,LV_TEXT_ALIGN_CENTER,0);
  } else if(page==Foods||page==Toys) {
    topRow(false);
    if(page==Foods) {
      const lv_img_dsc_t *foods[]={&icon_food0,&icon_food1,&icon_food2,&icon_food3,&icon_food4,&icon_food5};
      for(unsigned i=0;i<6;i++) iconButton(screen,foods[i],54+(i%3)*130,110+(i/3)*136,112,112,Feed0+i,0xffe8cc);
    } else {
      iconButton(screen,&icon_toy0,80,160,140,140,Play0,0xc0e5e2);
      iconButton(screen,&icon_toy1,260,160,140,140,Play0+1,0xc0e5e2);
    }
  } else if(page==Rooms) {
    topRow(false);
    // Up to 8 thumbnails, 4 per row, rows centred vertically between the top row and the corners.
    const unsigned count=pack?pack->backgroundCount:0,cols=std::min(count,4u),rows=(count+3)/4;
    const int top=rows>1?96:150;
    for(unsigned i=0;i<count;i++) {
      const unsigned inRow=i/4<rows-1?cols:count-(rows-1)*4;
      const int left=(480-int(inRow)*104-(int(inRow)-1)*10)/2;
      auto b=lv_btn_create(screen);lv_obj_set_pos(b,left+(i%4)*114,top+(i/4)*120);lv_obj_set_size(b,104,104);
      lv_obj_set_style_radius(b,18,0);lv_obj_set_style_pad_all(b,0,0);lv_obj_set_style_shadow_width(b,0,0);
      lv_obj_set_style_bg_color(b,color(0xffffff),0);lv_obj_set_style_clip_corner(b,true,0);
      lv_obj_set_style_border_width(b,i==pack->selected?4:2,0);
      lv_obj_set_style_border_color(b,color(i==pack->selected?0xff8fab:0xe0d8cc),0);
      lv_obj_add_event_cb(b,event,LV_EVENT_CLICKED,reinterpret_cast<void *>(static_cast<intptr_t>(Room0+i)));
      if(pack->thumbs[i].data) {
        auto &t=thumbImages[i];t={};t.header.cf=LV_IMG_CF_TRUE_COLOR;
        t.header.w=pack->thumbHeaders[i].width;t.header.h=pack->thumbHeaders[i].height;
        t.data_size=t.header.w*t.header.h*2;t.data=pack->thumbs[i].data+16;
        auto img=lv_img_create(b);lv_img_set_src(img,&t);lv_obj_center(img);
      }
    }
  } else if(page==Settings) {
    topRow(false);
    auto panel=settingsPanel=lv_obj_create(screen);lv_obj_set_pos(panel,24,78);lv_obj_set_size(panel,432,382);
    lv_obj_set_style_pad_all(panel,0,0);lv_obj_set_style_border_width(panel,0,0);lv_obj_set_style_radius(panel,32,0);
    lv_obj_set_style_clip_corner(panel,true,0);lv_obj_set_style_bg_color(panel,color(0xfffaf3),0);
    const lv_img_dsc_t *rowIcons[]={&icon_set_bright,&icon_set_volume};
    const char *rowNames[]={"亮度","音量"};
    for(int i=0;i<2;i++) {
      settingRow(panel,i*66,rowIcons[i],rowNames[i]);
      iconButton(panel,&icon_minus,236,i*66+9,76,48,i?VolumeDown:BrightnessDown,0xf3efe8);
      iconButton(panel,&icon_plus,324,i*66+9,76,48,i?VolumeUp:BrightnessUp,0xf3efe8);
    }
    switchRow(panel,132,&icon_set_autooff,"自動熄屏",pet.autoOff,AutoOff);
    switchRow(panel,198,&icon_set_dim,"自動調暗",!pet.keepBright,AutoDim);
    switchRow(panel,264,&icon_set_night,"夜晚靜音",pet.nightMute,NightMute);
    settingRow(panel,330,&icon_set_clock,"時間格式");button(panel,pet.hour24?"24H":"12H",280,339,120,48,Hour24,0xf3efe8);
    char tmp[16];snprintf(tmp,sizeof(tmp),"%uH",pet.offlineHours);
    settingRow(panel,396,&icon_set_offline,"離線計算上限");button(panel,tmp,280,405,120,48,OfflineCap,0xf3efe8);
    // Actions: full-width buttons with icon and name.
    const lv_img_dsc_t *actions[]={&icon_set_clock,&icon_set_sync,&icon_set_reload,&icon_set_info};
    const char *actionNames[]={"設定日期時間","Wi-Fi 設定","重新載入素材","狀態 / 版本"};
    const int ids[]={EditTime,Sync,Reload,GoStatus};
    for(int i=0;i<4;i++) {
      auto b=iconButton(panel,actions[i],12,468+i*64,408,56,ids[i],0xede7fb);
      lv_obj_align(lv_obj_get_child(b,0),LV_ALIGN_LEFT_MID,18,0);
      label(b,actionNames[i],66,12,320);
    }
    label(panel,chargeConfigured?"充電 4.1V / 300mA":"充電停用",24,728,384,0x7a6e7f);
    if(saveLocked) button(panel,"資料損壞：確認重設",20,770,392,60,ResetConfirm,0xffc7c7);
    box(panel,0,saveLocked?830:770,10,20,0xfffaf3,0);  // bottom margin when scrolled to the end
    lv_obj_update_layout(panel);lv_obj_scroll_to_y(panel,keepScroll,LV_ANIM_OFF);
  } else if(page==Status) {
    topRow(false);
    infoLabel=label(screen,"",48,84,384);
  } else if(page==SetTime) {
    topRow(false);
    label(screen,"香港時間 UTC+8",140,28,240);
    for(unsigned i=0;i<5;i++) {
      timeFields[i]=label(screen,"",44+i*80,140,76);
      lv_obj_set_style_text_align(timeFields[i],LV_TEXT_ALIGN_CENTER,0);
    }
    const lv_img_dsc_t *icons[]={&icon_prev,&icon_next,&icon_minus,&icon_plus};
    const int ids[]={TimePrev,TimeNext,TimeMinus,TimePlus};
    for(int i=0;i<4;i++) iconButton(screen,icons[i],48+i*98,214,88,60,ids[i],0xf7f3ed);
    iconButton(screen,&icon_ok,150,312,180,64,TimeSave,0xc0e5e2);
  } else if(page==WifiSetup) {
    topRow(false);
    // Phone scans the QR to join CHIMMY's own Wi-Fi; the setup page then opens by itself.
    auto title=label(screen,"Wi-Fi 設定",90,72,300);lv_obj_set_style_text_align(title,LV_TEXT_ALIGN_CENTER,0);
    if(net::startPortal()) {
      auto qr=lv_qrcode_create(screen,196,color(0x2b2530),color(0xffffff));
      lv_obj_set_pos(qr,142,112);lv_obj_set_style_border_color(qr,color(0xffffff),0);lv_obj_set_style_border_width(qr,8,0);
      lv_qrcode_update(qr,net::portalQr(),strlen(net::portalQr()));
    }
    infoLabel=label(screen,"",60,328,360);lv_obj_set_style_text_align(infoLabel,LV_TEXT_ALIGN_CENTER,0);
  }
  toast=label(screen,toastMessage,70,82,340,0x594761);
  lv_obj_set_style_text_align(toast,LV_TEXT_ALIGN_CENTER,0);lv_obj_set_style_radius(toast,14,0);
  lv_obj_set_style_pad_all(toast,6,0);
  lv_obj_set_style_bg_color(toast,color(0xfff0cf),0);lv_obj_set_style_bg_opa(toast,LV_OPA_90,0);
  if(elapsed(toastUntil)) lv_obj_add_flag(toast,LV_OBJ_FLAG_HIDDEN);
}
// Full and tired refusals are shown by the refuse animation; only the sleeping case needs words.
static void resultMessage(game::Result result) {
  if(result==game::Result::Sleeping) notify("請先起床");
  else if(result!=game::Result::Full&&result!=game::Result::Tired) notify("操作無效");
}
// Full or tired: return to the pet and play the refuse clip. A sleeping pet stays asleep.
static void closeSheet() {if(sheet) {lv_obj_del(sheet);sheet=nullptr;}}
static void toRoom() {if(page!=Ambient) showPage(Ambient); else closeSheet();}
static void refuse(game::Result result) {
  if((result==game::Result::Full||result==game::Result::Tired)&&!pet.sleeping) {toRoom();setAction(6);}
  resultMessage(result);
}
static bool canAct() {
  if(saveLocked) {notify("資料損壞：請先復原");return false;}
  if(transfer.active||assetLoading) {notify("載入中");return false;}
  return resting(action);
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
  if(!rtc) notify("時間已設定，RTC寫入失敗",6000);
  return true;
}
// Care menu over the room: six snacks, then ball, games, rooms and close. Bottom corners
// are rounded like the panel; everything stays inside x=33..447.
static void exitView(lv_event_t *) {
  if(!viewing) return;
  lastInput=millis();viewing=false;
  lv_obj_del_async(viewPad);viewPad=nullptr;
  for(unsigned i=0;i<viewHiddenCount;i++) lv_obj_clear_flag(viewHidden[i],LV_OBJ_FLAG_HIDDEN);
  viewHiddenCount=0;lv_obj_clear_flag(lv_layer_top(),LV_OBJ_FLAG_HIDDEN);
  if(character) {
    lv_img_set_zoom(character,256);
    wanderY=targetY=std::clamp<float>(wanderY,wanderMinY(),wanderMaxY());  // back onto the normal floor
    if(step.kind==behavior::Kind::Walk) targetX=wanderX;
  }
}
static void enterView() {
  if(viewing||page!=Ambient||!character) return;
  closeSheet();viewing=true;charZoom=kViewZoom;viewHiddenCount=0;
  // Hide every visible control; the background and CHIMMY stay. The status layer goes too.
  const uint32_t n=lv_obj_get_child_cnt(screen);
  for(uint32_t i=0;i<n&&viewHiddenCount<48;i++) {
    lv_obj_t *o=lv_obj_get_child(screen,i);
    if(o==background||o==character||lv_obj_has_flag(o,LV_OBJ_FLAG_HIDDEN)) continue;
    lv_obj_add_flag(o,LV_OBJ_FLAG_HIDDEN);viewHidden[viewHiddenCount++]=o;
  }
  lv_obj_add_flag(lv_layer_top(),LV_OBJ_FLAG_HIDDEN);
  lv_img_set_zoom(character,kViewZoom);
  wanderY=targetY=std::clamp<float>(wanderY,wanderMinY(),wanderMaxY());
  if(step.kind==behavior::Kind::Walk) targetX=wanderX;
  // A touch anywhere (CHIMMY included) ends viewing.
  viewPad=lv_obj_create(screen);lv_obj_set_pos(viewPad,0,0);lv_obj_set_size(viewPad,480,480);
  lv_obj_set_style_bg_opa(viewPad,LV_OPA_TRANSP,0);lv_obj_set_style_border_width(viewPad,0,0);
  lv_obj_clear_flag(viewPad,LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(viewPad,exitView,LV_EVENT_CLICKED,nullptr);
}
static void openSheet() {
  if(sheet||page!=Ambient) return;
  sheet=box(screen,24,292,432,176,0xfffaf3,40);lv_obj_set_style_bg_opa(sheet,LV_OPA_90,0);
  lv_obj_add_flag(sheet,LV_OBJ_FLAG_CLICKABLE);  // taps on the panel do not reach the room
  const lv_img_dsc_t *foods[]={&icon_m_food0,&icon_m_food1,&icon_m_food2,&icon_m_food3,&icon_m_food4,&icon_m_food5};
  for(int i=0;i<6;i++) iconButton(sheet,foods[i],9+i*70,14,64,64,Feed0+i,0xffe8cc);
  const lv_img_dsc_t *row[]={&icon_m_outfit,&icon_m_game,&icon_m_room,&icon_close};
  const int ids[]={GoOutfits,GoGames,GoRooms,ToggleSheet};
  const uint32_t tints[]={0xfff1b8,0xdff4ff,0xded6f5,0xf3efe8};
  for(int i=0;i<4;i++) iconButton(sheet,row[i],44+i*92,92,64,64,ids[i],tints[i]);
}
static void showFood(unsigned i) {
  const lv_img_dsc_t *foods[]={&icon_m_food0,&icon_m_food1,&icon_m_food2,&icon_m_food3,&icon_m_food4,&icon_m_food5};
  if(foodSprite) lv_obj_del(foodSprite);
  foodSprite=lv_img_create(screen);lv_img_set_src(foodSprite,foods[i%6]);
  lv_obj_set_pos(foodSprite,-100,-100);  // placed next frame, beside the moving character
}
// Little hearts float up from CHIMMY's head and fade (touch feedback, care rewards).
static void spawnHearts(unsigned n) {
  if(!character) return;
  for(unsigned i=0;i<n;i++) {
    auto h=lv_img_create(screen);lv_img_set_src(h,&icon_fx_heart);
    const int x=lv_obj_get_x(character)+charSize()/2-13+int(esp_random()%90)-45,y=lv_obj_get_y(character)+40;
    lv_obj_set_pos(h,x,y);lv_obj_clear_flag(h,LV_OBJ_FLAG_CLICKABLE);
    lv_anim_t a;lv_anim_init(&a);lv_anim_set_var(&a,h);lv_anim_set_delay(&a,i*140);lv_anim_set_time(&a,1000);
    lv_anim_set_values(&a,y,y-70);
    lv_anim_set_exec_cb(&a,[](void *o,int32_t v){lv_obj_set_y(static_cast<lv_obj_t *>(o),v);});
    lv_anim_set_ready_cb(&a,[](lv_anim_t *x){lv_obj_del_async(static_cast<lv_obj_t *>(x->var));});
    lv_anim_start(&a);
    lv_anim_t f;lv_anim_init(&f);lv_anim_set_var(&f,h);lv_anim_set_delay(&f,i*140+500);lv_anim_set_time(&f,500);
    lv_anim_set_values(&f,LV_OPA_COVER,LV_OPA_TRANSP);
    lv_anim_set_exec_cb(&f,[](void *o,int32_t v){lv_obj_set_style_opa(static_cast<lv_obj_t *>(o),v,0);});
    lv_anim_start(&f);
  }
}
static void control(int id) {
  if(transfer.active) {notify("USB素材傳輸中");return;}
  if(id>=Feed0&&id<Feed0+6) {
    if(!canAct()) return;advanceNow();
    const auto result=game::feed(pet,id-Feed0);
    if(result!=game::Result::Ok) {refuse(result);return;}
    // Eaten in place with the snack in hand; the favourite earns a happy hop and more hearts.
    const unsigned food=id-Feed0;const bool favorite=food==pet.favoriteFood;
    changed();toRoom();setAction(2);showFood(food);spawnHearts(favorite?5:2);
    if(favorite) queuedAction=7;
    return;
  }
  if(id==Play0+1) {closeSheet();showPage(GamesHub);return;}  // was the bubble round
  if(id>=PlayGame0&&id<PlayGame0+int(games::kGames)) {
    if(!pack) {notify("示範素材：使用內建圖形，請由USB安裝素材",6000);return;}
    playing=games::Id(id-PlayGame0);showPage(Play);return;
  }
  if(id>=Outfit0&&id<Outfit0+int(kMaxOutfits)) {
    const unsigned i=id-Outfit0;
    if(!pack||i>=pack->outfitCount||!canAct()) return;
    if(i==pack->outfit) {showPage(Ambient);setAction(5);return;}
    if(assetLoading) {notify("載入中，請稍等");return;}
    pet.outfit=i;changed();
    // Two atlases do not fit in PSRAM: drop the worn one first; the stand-in shows while loading.
    stopSound();pack.reset();lv_img_cache_invalidate_src(nullptr);
    showPage(Ambient);dressAfterLoad=true;requestAssets();
    return;
  }
  if(id==Play0) {
    if(!canAct()) return;
    if(pet.sleeping) {notify("請先起床");return;}
    if(pet.energy<800) {refuse(game::Result::Tired);return;}
    advanceNow();const auto result=game::play(pet,0);
    if(result!=game::Result::Ok){refuse(result);return;}
    changed();toRoom();setAction(3);spawnHearts(2);
    return;
  }
  if(id>=Room0&&id<Room0+int(kMaxBackgrounds)) {
    if(!canAct()) return;
    pet.background=id-Room0;changed();showPage(Game);requestAssets();setAction(5);return;
  }
  switch(id) {
    case GoGame:showPage(Ambient);break;
    case ToggleSheet:if(sheet) closeSheet(); else openSheet();break;
    case GoAmbient:showPage(Ambient);break;
    case GoSettings:showPage(Settings);break;
    case GoFoods:if(canAct())showPage(Foods);break;
    case GoToys:if(canAct())showPage(Toys);break;
    case GoRooms:if(canAct())showPage(Rooms);break;
    case GoStatus:showPage(Status);break;
    case Tap:
      if((page<=Ambient||page==Bus)&&canAct()) {
        if(!pet.sleeping) spawnHearts(1);  // a heart says "got it" before the reaction
        // A random reaction, never the same one twice in a row.
        if(pet.sleeping) setAction(4);
        else if(pack&&pack->tapCount) {
          int i=esp_random()%pack->tapCount;
          if(i==lastTap&&pack->tapCount>1) i=(i+1)%pack->tapCount;
          lastTap=i;setAction(ActTap0+i);
        } else setAction(pet.bond>=40?7:1);
      }
      break;
    case Back:showPage(page==Status||page==SetTime||page==WifiSetup?Settings:page==Bus?Ambient:page==BusSetup?Bus:Game);break;
    case GoBus:showPage(Bus);break;
    case GoBusSetup:showPage(BusSetup);break;
    case Sleep:
      // The moon/sun button: a manual sleep is not woken at 7:00 (only the pet's own night sleep is).
      if(!canAct())break;
      if(pet.sleeping) wakeNow(); else sleepNow(false);
      break;
    case BrightnessDown:pet.brightness=std::max<int>(20,pet.brightness-20);changed();display.setBrightness(pet.brightness);break;
    case BrightnessUp:pet.brightness=std::min<int>(220,pet.brightness+20);changed();display.setBrightness(pet.brightness);break;
    case VolumeDown:pet.volume=std::max<int>(0,pet.volume-10);changed();break;
    case VolumeUp:pet.volume=std::min<int>(100,pet.volume+10);changed();soundFor(1);break;
    // Switches show their own state, so these two do not rebuild the page.
    case AutoOff:pet.autoOff=!pet.autoOff;changed();break;
    case Hour24:pet.hour24=!pet.hour24;changed();showPage(Settings);break;
    case NightMute:pet.nightMute=!pet.nightMute;changed();break;
    case AutoDim:pet.keepBright=!pet.keepBright;changed();break;
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
      else showPage(Settings);break;
    }
    case Reload:requestAssets();break;
    case Sync:showPage(WifiSetup);break;
    case GoGames:closeSheet();showPage(GamesHub);break;
    case GoOutfits:closeSheet();showPage(Outfits);break;
    case ViewMode:enterView();break;
    case ResetConfirm:
      showPage(Status);button(screen,"重設育成資料？",70,340,340,64,ResetYes,0xffc7c7);break;
    case ResetYes:
      if(!saveLocked)break;
      if(!saves.reset(pet,nowUtc())){notify("重設失敗：請檢查裝置",10000);break;}
      saveLocked=false;dirty=false;urgentSave=false;lastSave=millis();lastTickUs=esp_timer_get_time();
      showPage(Game);notify("資料已重設");break;
  }
}
static void updateAnimation() {
  if((page!=Game&&page!=Ambient&&page!=Bus)||!displayOn||transfer.active) return;
  if(pack&&character) wanderTick();
  // After wanderTick: a step that just started resets actionAt, and a stale delta would end it at once.
  const uint32_t delta=millis()-actionAt;
  if(pack&&character) {
    const Clip &c=clipFor(action);uint32_t total=0;for(unsigned i=0;i<c.count;i++)total+=c.durations[i];
    if(!total)return;
    if(!c.loop&&delta>=total) {
      const int next=queuedAction>=0&&!pet.sleeping?queuedAction:pet.sleeping?4:0;
      queuedAction=-1;setAction(next);return;
    }
    uint32_t phase=delta%total;unsigned index=0;
    while(index+1<c.count&&phase>=c.durations[index]) {phase-=c.durations[index];index++;}
    drawFrame(c.frames[index]);
    const bool walking=step.kind==behavior::Kind::Walk&&resting(action)&&!pet.sleeping;
    const bool lively=action==3||action==7||(action>=ActTap0&&action<ActWalk);
    // Walking hops a little on each step; sitting and sleeping stay still.
    const int bounce=walking?-int(std::fabs(std::sin(millis()/170.0))*7):
      action==ActSit||action==4?0:int(std::sin(delta/220.0)*(lively?8:2));
    const bool home=pet.sleeping;  // asleep: stays at the centre spot
    lv_obj_set_pos(character,home?restX():int(wanderX),(home?homeY():int(wanderY))+bounce);
    if(foodSprite) {
      // In both hands at chest height, gone after most of the eating clip.
      if(action!=2||delta>total*7/10) {lv_obj_del(foodSprite);foodSprite=nullptr;}
      else lv_obj_set_pos(foodSprite,lv_obj_get_x(character)+charSize()/2-26,lv_obj_get_y(character)+charSize()*58/100);
    }
  } else if(fallback) {
    if(action!=0&&action!=4&&delta>1500) setAction(pet.sleeping?4:0);
    const bool blink=pet.sleeping||(millis()%3000<160);
    for(auto eye:eyes)lv_obj_set_height(eye,blink?4:18);
    lv_obj_set_y(fallback,105+int(std::sin(millis()/250.0)*(action?6:2)));
    frameCount++;
  }
}
// Bus screen: fetch every 30s (at once after the phone saves new lines); minutes count down locally.
static void busTick() {
  const bool online=net::state()==net::State::Connected;
  const bool changed=busRevision!=bus::revision();
  if(online&&!kmb::busy()&&(changed||millis()-busFetchAt>30000)) {
    kmb::Request q[bus::kLines]{};unsigned n=0;
    for(unsigned i=0;i<bus::kLines;i++) {
      busBoard[i]=-1;if(!bus::used(i)) continue;
      const auto &l=bus::line(i);auto &r=q[n];
      snprintf(r.route,sizeof(r.route),"%s",l.route);snprintf(r.stop,sizeof(r.stop),"%s",l.stop);
      r.bound=l.bound;r.serviceType=l.serviceType;busBoard[i]=n++;
    }
    if(changed) busMapAt=millis();  // older batches no longer match the lines
    if(!n||kmb::start(q,n)) {busFetchAt=millis();busRevision=bus::revision();}
  }
  const auto r=kmb::last();
  const bool fresh=r.done&&int32_t(r.finishedMs-busMapAt)>=0;
  const int64_t now=nowUtc();
  for(unsigned i=0;i<bus::kLines;i++) {
    if(!busEta[i]||!bus::used(i)) continue;
    char b[96]="等緊資料…";
    if(!online) snprintf(b,sizeof(b),"Wi-Fi 未連接");
    else if(fresh&&busBoard[i]>=0&&busBoard[i]<r.boardCount) {
      const auto &bd=r.boards[busBoard[i]];
      if(!bd.ok) snprintf(b,sizeof(b),"暫時攞唔到資料");
      else if(!bd.count) snprintf(b,sizeof(b),"暫時冇班次");
      else if(!bd.arrivals[0].eta) snprintf(b,sizeof(b),"%s",bd.arrivals[0].remark[0]?bd.arrivals[0].remark:"暫時冇班次");
      else {
        // First bus in full ("6 分鐘" / "即將到站"), then the later ones as bare minutes.
        int used=0;
        for(unsigned k=0;k<bd.count&&bd.arrivals[k].eta;k++) {
          const int mins=int((bd.arrivals[k].eta-now+59)/60);
          if(!k) used=mins<=0?snprintf(b,sizeof(b),"即將到站"):snprintf(b,sizeof(b),"%d 分鐘",mins);
          else used+=snprintf(b+used,sizeof(b)-used," · %d",std::max(0,mins));
          if(used>=int(sizeof(b))) break;
        }
      }
    }
    if(strcmp(lv_label_get_text(busEta[i]),b)) lv_label_set_text(busEta[i],b);
  }
}
static void updateLabels() {
  const int64_t utc=nowUtc();time_t epoch=utc;tm local{};
  if(utc)localtime_r(&epoch,&local);
  if(clockLabel) {
    char buf[40]="--:--";
    if(utc)snprintf(buf,sizeof(buf),"%02d:%02d",pet.hour24?local.tm_hour:((local.tm_hour+11)%12+1),local.tm_min);
    lv_label_set_text(clockLabel,buf);
  }
  if(dateLabel) {
    static const char *days[]={"日","一","二","三","四","五","六"};
    char buf[40]="";
    if(utc)snprintf(buf,sizeof(buf),"%d月%d日 星期%s",local.tm_mon+1,local.tm_mday,days[local.tm_wday]);
    if(strcmp(lv_label_get_text(dateLabel),buf))lv_label_set_text(dateLabel,buf);
  }
  if(page==Bus) busTick();
  if(page==Ambient) {
    const int values[]={pet.hunger,pet.mood,pet.energy};
    for(int i=0;i<3;i++)if(stats[i])lv_bar_set_value(stats[i],values[i]/100,LV_ANIM_OFF);
    if(bondLabel) {char b[12];snprintf(b,sizeof(b),"%lu",(unsigned long)std::min<uint32_t>(pet.bond,999));
      if(strcmp(lv_label_get_text(bondLabel),b))lv_label_set_text(bondLabel,b);}
  }
  if(infoLabel&&page==WifiSetup) {
    char b[160];const auto j=net::joinState();
    if(!net::portalActive()&&j!=net::Join::Joined) snprintf(b,sizeof(b),"開唔到設定，請返回再試");
    else if(j==net::Join::Trying) snprintf(b,sizeof(b),"連接緊…");
    else if(j==net::Join::Joined) snprintf(b,sizeof(b),"已連接：%s",net::ssid().c_str());
    else if(j==net::Join::Rejected) snprintf(b,sizeof(b),"連唔到，請檢查密碼再試");
    else snprintf(b,sizeof(b),"用手機掃描 QR Code\n連接後會自動打開設定頁");
    lv_label_set_text(infoLabel,b);
  }
  if(infoLabel&&page==Status) {
    char b[600];
    char batteryText[32];battery::format(batteryReading,batteryText,sizeof(batteryText));
    snprintf(b,sizeof(b),"CHIMMY GOCHI 0.2\n親密 %lu  /  睡意 %ld\n亮度 %u  /  音量 %u\nSD %s  /  RTC %s\n觸控 %s  /  音效 %s\n電量 %s %s  /  %s\n保存 %llu  /  %s\nPSRAM %u KB",
      (unsigned long)pet.bond,(long)pet.sleepy/100,pet.brightness,pet.volume,
      assets.ready?"OK":"ERROR",clockQuality?"OK":"--",touchOk?"OK":"ERROR",audioOk?"OK":"ERROR",
      batteryText,usbPower?"USB":"",chargeHot?"充電暫停":chargeConfigured?"充電 4.1V":"充電停用",(unsigned long long)pet.generation,saveLocked?"ERROR":dirty?"保存中":"已保存",ESP.getFreePsram()/1024);
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
    else display.setBrightness(idle>30000&&!pet.keepBright?std::max<int>(20,pet.brightness/3):pet.brightness);
  }
}
// Wi-Fi stays up (remembered networks); every new connection also re-syncs the clock over NTP.
static void networkTick() {
  net::tick();
  static bool requested=false;
  if(net::connectedOnce()) {
    configTime(0,0,"pool.ntp.org","time.cloudflare.com");setenv("TZ","HKT-8",1);tzset();requested=true;
  }
  // SNTP sync status, rather than an already valid RTC time, determines success.
  extern bool networkSynced();
  if(requested&&networkSynced()) {
    requested=false;clockQuality=3;const bool rtc=rtcWrite(nowUtc());changed();
    if(!rtc)notify("時間已同步，RTC寫入失敗");
  }
}
#include <esp_sntp.h>
bool networkSynced(){return sntp_get_sync_status()==SNTP_SYNC_STATUS_COMPLETED;}

static void command(JsonDocument &request,JsonDocument &reply) {
  const char *cmd=request["cmd"]|"";
  String error;
  bool ok=false;
  if(strcmp(cmd,"status")==0) {
    ok=true;reply["version"]="0.2.0";reply["page"]=page;reply["sd"]=assets.ready;
    reply["psram"]=ESP.getPsramSize();reply["psram_free"]=ESP.getFreePsram();reply["heap"]=ESP.getFreeHeap();
    reply["touch"]=touchOk;reply["audio"]=audioOk;reply["pmic"]=pmuOk;reply["charge_configured"]=chargeConfigured;reply["charge_paused_hot"]=chargeHot;
    reply["pmic_temp_c"]=pmuOk?pmu.getTemperature():0;reply["battery_mv"]=pmuOk?pmu.getBattVoltage():0;
    reply["battery_state"]=battery::stateName(batteryReading.state);
    if(batteryReading.state==battery::State::Ready)reply["battery_percent"]=batteryReading.percent;
    else reply["battery_percent"]=nullptr;
    reply["battery_text"]=batteryLabel?lv_label_get_text(batteryLabel):"";
    reply["battery_visible"]=batteryLabel&&!lv_obj_has_flag(batteryLabel,LV_OBJ_FLAG_HIDDEN);
    if(batteryLabel) {
      lv_obj_update_layout(batteryLabel);
      const auto font=lv_obj_get_style_text_font(batteryLabel,LV_PART_MAIN);
      const auto spacing=lv_obj_get_style_text_letter_space(batteryLabel,LV_PART_MAIN);
      const char *widest="100%";
      reply["battery_text_width_px"]=lv_txt_get_width(lv_label_get_text(batteryLabel),
        strlen(lv_label_get_text(batteryLabel)),font,spacing,LV_TEXT_FLAG_NONE);
      reply["battery_max_width_px"]=lv_txt_get_width(widest,strlen(widest),font,spacing,LV_TEXT_FLAG_NONE);
      reply["battery_content_width_px"]=lv_obj_get_content_width(batteryLabel);
      reply["battery_clickable"]=lv_obj_has_flag(batteryBox,LV_OBJ_FLAG_CLICKABLE)||
        lv_obj_has_flag(batteryLabel,LV_OBJ_FLAG_CLICKABLE);
    }
    reply["wifi_state"]=int(net::state());reply["wifi_ssid"]=net::ssid();reply["wifi_rssi"]=net::rssi();
    reply["wifi_saved"]=net::savedCount();reply["portal"]=net::portalActive();reply["portal_join"]=int(net::joinState());
    reply["battery_sample_ms"]=batteryReadAt;reply["usb_power"]=usbPower;reply["charging"]=charging;
    reply["utc"]=nowUtc();int64_t rtc=0;reply["rtc_valid"]=rtcRead(rtc);reply["rtc_utc"]=rtc;
    reply["save_generation"]=pet.generation;reply["save_locked"]=saveLocked;reply["dirty"]=dirty;
    reply["background"]=pet.background;reply["background_count"]=pack?pack->backgroundCount:0;
    reply["outfit"]=pet.outfit;reply["outfit_count"]=pack?pack->outfitCount:0;
    reply["hunger"]=pet.hunger;reply["mood"]=pet.mood;reply["energy"]=pet.energy;
    reply["sleeping"]=pet.sleeping;reply["bond"]=pet.bond;reply["flushes"]=flushCount;
    reply["action"]=action;reply["step_kind"]=int(step.kind);reply["step_ms"]=step.ms;reply["step_age"]=millis()-stepAt;
    reply["idle_ms"]=millis()-lastInput;reply["gestures"]=pack?pack->gestureMask:0;reply["game"]=page==Play?int(playing):-1;
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
       (value>=Play0&&value<Play0+2)||(value>=Room0&&value<Room0+int(kMaxBackgrounds))||(value>=BrightnessDown&&value<=ResetConfirm)||(value>=GoBus&&value<=GoOutfits)||(value>=PlayGame0&&value<PlayGame0+int(games::kGames))||(value>=Outfit0&&value<Outfit0+int(kMaxOutfits))))
      error="invalid control";
    else {lastInput=millis();control(value);ok=true;}
  } else if(strcmp(cmd,"wifi")==0) {
    const char *ssid=request["ssid"],*password=request["password"];
    if(!ssid||!*ssid||strlen(ssid)>32||!password||strlen(password)>63)error="invalid WiFi configuration";
    else ok=net::addNetwork(ssid,password);
  } else if(strcmp(cmd,"kmb")==0) {
    // Diagnostic fetch of one stop: {stop, route, service_type, bound}; poll with kmb_result.
    kmb::Request q{};snprintf(q.route,sizeof(q.route),"%s",request["route"]|"");
    snprintf(q.stop,sizeof(q.stop),"%s",request["stop"]|"");q.serviceType=request["service_type"]|1;
    const char *b=request["bound"]|"";q.bound=b[0];
    ok=kmb::start(&q,1);
    if(!ok)error="kmb busy, offline or invalid stop/route";
  } else if(strcmp(cmd,"kmb_result")==0) {
    // Not const: ArduinoJson copies char* strings but stores const char* by pointer, and r dies first.
    auto r=kmb::last();ok=true;
    reply["busy"]=kmb::busy();reply["done"]=r.done;reply["request_ms"]=r.requestMs;
    reply["heap_before"]=r.heapBefore;reply["heap_low_after"]=r.heapLowAfter;reply["stack_free"]=r.stackFree;
    reply["max_loop_us"]=maxLoopUs;
    JsonArray boards=reply["boards"].to<JsonArray>();
    for(unsigned b=0;b<r.boardCount;b++) {
      auto &bd=r.boards[b];JsonObject o=boards.add<JsonObject>();
      o["ok"]=bd.ok;o["http"]=bd.http;o["error"]=(char *)bd.error;
      JsonArray list=o["arrivals"].to<JsonArray>();
      for(unsigned i=0;i<bd.count;i++) {
        JsonObject a=list.add<JsonObject>();a["dest"]=(char *)bd.arrivals[i].dest;a["remark"]=(char *)bd.arrivals[i].remark;
        a["minutes"]=bd.arrivals[i].eta?(bd.arrivals[i].eta-nowUtc())/60:-1;
      }
    }
  } else if(strcmp(cmd,"sd_bench")==0) {
    // v0.3 spike S3: read the pack atlas in frame-sized chunks.
    const size_t chunk=request["chunk"]|248832;const unsigned count=request["count"]|4;
    const String path=String(pack?pack->root:"")+"/chimmy.cga";
    const uint32_t us=assets.benchRead(path.c_str(),chunk,count);
    ok=us>0;if(!ok)error="bench read failed";
    reply["us"]=us;reply["bytes"]=uint32_t(chunk*count);reply["kb_per_s"]=us?uint32_t(uint64_t(chunk)*count*1000000/1024/us):0;
    reply["ms_per_chunk"]=us/1000.0/count;
  } else if(strcmp(cmd,"reload")==0) {requestAssets();ok=assetLoading;if(!ok)error="cannot load pack";}
  else if(strcmp(cmd,"pack_begin")==0) {
    if(assetLoading)error="asset worker busy";
    else {ok=transfer.start(request["name"]|"",error);if(ok){stopSound();notify("USB素材傳輸中",60000);}}
  } else if(strcmp(cmd,"file_begin")==0)
    ok=transfer.open(request["name"]|"",request["size"]|0,request["sha256"]|"",error);
  else if(strcmp(cmd,"chunk")==0)
    ok=transfer.chunk(request["offset"]|SIZE_MAX,request["hex"]|"",error);
  else if(strcmp(cmd,"file_end")==0) ok=transfer.finish(error);
  else if(strcmp(cmd,"pack_end")==0) {
    // Verifying holds a full atlas; drop the one on screen first so PSRAM never needs two.
    stopSound();lv_img_cache_invalidate_src(nullptr);pack.reset();showPage(page);
    ok=transfer.complete(error);requestAssets();
  }
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
  game::offline(pet,nowUtc());
  lastTickUs=esp_timer_get_time();lastInput=millis();
  Serial.println("INIT SD");
  engine=behavior::Engine(esp_random());
  if(pet.favoriteFood>=6) {pet.favoriteFood=esp_random()%6;changed();}  // this pet's favourite snack
  net::begin();bus::begin();bus::attach(net::web());
  assets.begin();showPage(Ambient);
  if(saveLocked)notify("資料損壞或保存初始化失敗：請檢查設定",15000);
  else {
    saveNow();
    if(loaded==SaveStore::Load::Recovered)notify("已復原上一份保存資料",10000);
  }
  requestAssets();
  Serial.printf("READY display=1 touch=%d audio=%d pmu=%d SD=%d psram=%u RTC=%d saveLocked=%d\n",
    touchOk,audioOk,pmuOk,assets.ready,ESP.getPsramSize(),clockQuality,saveLocked);
}
void loop() {
  const uint64_t start=esp_timer_get_time();
  advanceNow();updateBattery();serialTick();powerTick();networkTick();
  static uint32_t lastSleepCheck=0;
  if(millis()-lastSleepCheck>=1000) {lastSleepCheck=millis();sleepTick();}
  AssetResult loaded;
  if(assets.poll(loaded)) {
    assetLoading=false;
    if(loaded.generation!=sceneGeneration)delete loaded.pack;
    else if(!loaded.pack) {
      if(pack&&pet.background!=pack->selected){pet.background=pack->selected;changed();}
      showPage(page);notify(loaded.error,10000);
      if(!pack&&pet.outfit) {pet.outfit=0;changed();dressAfterLoad=false;requestAssets();}  // back to everyday clothes
    }
    else {
      stopSound();
      lv_img_cache_invalidate_src(nullptr);
      auto oldPack=std::move(pack);
      pack.reset(loaded.pack);
      if(pet.background!=pack->selected){pet.background=pack->selected;changed();}  // pack has fewer rooms
      if(pet.outfit!=pack->outfit){pet.outfit=pack->outfit;changed();}  // or fewer outfits
      showPage(page);
      if(dressAfterLoad) {dressAfterLoad=false;if(page==Ambient)setAction(5);}
    }
  }
  if(!capture&&millis()-lastFrameAt>=66) {lastFrameAt=millis();updateAnimation();}
  static uint32_t lastLabels=0;
  if(millis()-lastLabels>=250){lastLabels=millis();updateLabels();}
  if(dirty&&!saveLocked&&!transfer.active&&millis()-dirtyAt>=2000&&
     (urgentSave||millis()-lastSave>=60000||pet.generation<2)) saveNow();
  if(page==Play) games::tick();
  if(page==Game&&millis()-lastInput>45000&&!transfer.active)showPage(Ambient);
  if(capture&&millis()-captureAt>15000)capture=false;
  if(!capture)lv_timer_handler();
  if(audioFailures.load()!=audioFailureSeen) {
    audioFailureSeen=audioFailures.load();notify("音效播放失敗");
  }
  maxLoopUs=std::max<uint32_t>(maxLoopUs,esp_timer_get_time()-start);
  delay(3);
}
