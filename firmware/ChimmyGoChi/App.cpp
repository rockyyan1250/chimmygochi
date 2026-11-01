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
static lv_obj_t *clockLabel=nullptr,*infoLabel=nullptr,*stats[3]{},*settingsPanel=nullptr;
static lv_obj_t *batteryBox=nullptr,*batteryLabel=nullptr,*batteryFill=nullptr,*powerIcon=nullptr;
static battery::Reading batteryReading{battery::State::Unavailable,-1};
static bool batterySampled=false,usbPower=false,charging=false;
static lv_img_dsc_t thumbImages[kMaxBackgrounds]{};
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
// Actions 0..7 are the pack's named clips; these index its optional clips.
constexpr int ActTap0=8,ActWalk=ActTap0+int(kMaxTaps),ActSit=ActWalk+1;
// Standby screen: the pet rests, walks to a random spot, sometimes sits, and repeats.
enum class Wander : uint8_t { Rest, Walk, Sit };
static Wander wander=Wander::Rest;
static float wanderX=0,wanderY=0,targetX=0,targetY=0;
static uint32_t wanderUntil=0,lastWanderMs=0;
static int lastTap=-1;

// Page 2 and control 3 belonged to the removed clock mode; the numbers stay for the USB test tools.
enum Page { Game=0,Ambient=1,Settings=3,Foods=4,Toys=5,Rooms=6,Status=7,SetTime=8 };
enum Control { GoGame=1,GoAmbient,GoSettings=4,GoFoods,GoToys,GoRooms,Sleep,
  GoStatus,Back,Tap,Feed0=20,Play0=30,Room0=40,BrightnessDown=50,BrightnessUp,VolumeDown,VolumeUp,
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
static lv_obj_t *box(lv_obj_t *parent,int x,int y,int w,int h,uint32_t bg,int radius) {
  auto o=lv_obj_create(parent);lv_obj_set_pos(o,x,y);lv_obj_set_size(o,w,h);
  lv_obj_set_style_bg_color(o,color(bg),0);lv_obj_set_style_radius(o,radius,0);
  lv_obj_set_style_border_width(o,0,0);lv_obj_set_style_pad_all(o,0,0);
  lv_obj_clear_flag(o,LV_OBJ_FLAG_SCROLLABLE);lv_obj_clear_flag(o,LV_OBJ_FLAG_CLICKABLE);
  return o;
}
// Pill at the top centre (away from the panel's rounded corners): power mark, battery, percent.
static void initBatteryIndicator() {
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
  assetLoading=assets.request(root.c_str(),pet.background,++sceneGeneration,pack.get());
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
  return a==ActWalk?pack->walk:a==ActSit?pack->sit:pack->clips[a];
}
// Idle, sleep, walk and sit are resting states: new actions may interrupt them.
static bool resting(int a) {return a==0||a==4||a==ActWalk||a==ActSit;}
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
static int restX() {return (480-charSize())/2;}
static int restY() {return 352-charSize()+16;}
static int homeY() {return page==Game?gameY():restY();}
// Floor band per page: on the game page the feet stay above the stat chips (y~356).
static int wanderMinY() {return page==Game?gameY()-28:restY()-40;}
static int wanderMaxY() {return page==Game?gameY():restY()+28;}
static void resetWander() {
  wander=Wander::Rest;wanderUntil=millis()+2500;lastWanderMs=millis();
  wanderX=targetX=restX();wanderY=targetY=homeY();
}
static void wanderTick() {
  const uint32_t now=millis();
  const float dt=std::min<uint32_t>(now-lastWanderMs,200)/1000.0f;lastWanderMs=now;
  if(!resting(action)||pet.sleeping) return;  // a tap reaction pauses the walk
  if(wander==Wander::Walk) {
    if(pack->walk.count&&action!=ActWalk) setAction(ActWalk);
    const float dx=targetX-wanderX,dy=targetY-wanderY,dist=std::sqrt(dx*dx+dy*dy),step=28*dt;
    if(dist>step) {wanderX+=dx/dist*step;wanderY+=dy/dist*step;return;}
    wanderX=targetX;wanderY=targetY;
    if(pack->sit.count&&esp_random()%2) {wander=Wander::Sit;wanderUntil=now+6000+esp_random()%6000;setAction(ActSit);}
    else {wander=Wander::Rest;wanderUntil=now+3000+esp_random()%4000;setAction(0);}
  } else if(elapsed(wanderUntil)) {
    // The frame's transparent margin may leave the screen; the body stays inside. Each walk
    // covers at least 120px, turning back when the far side is too close.
    const int minX=-30,maxX=480-charSize()+30,minY=wanderMinY(),maxY=wanderMaxY();
    // int() keeps the negative minX out of unsigned arithmetic with esp_random().
    auto random=[](int n){return int(esp_random()%unsigned(n));};
    targetX=minX+random(maxX-minX+1);
    if(std::fabs(targetX-wanderX)<120) {
      const bool right=wanderX+120<=maxX&&(wanderX-120<minX||random(2));
      targetX=right?std::min<float>(maxX,wanderX+120+random(60)):std::max<float>(minX,wanderX-120-random(60));
    }
    targetX=std::max<float>(minX,std::min<float>(maxX,targetX));
    targetY=minY+random(maxY-minY+1);
    wander=Wander::Walk;if(action!=0&&!pack->walk.count) setAction(0);
  } else if(wander==Wander::Sit&&action!=ActSit) setAction(ActSit);
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
static void topRow(bool home) {
  if(home) {
    iconButton(screen,pet.sleeping?&icon_wake:&icon_sleep,70,12,48,48,Sleep,0xede7fb);
    iconButton(screen,&icon_nav_settings,362,12,48,48,GoSettings,0xf3f0ea);
  } else iconButton(screen,&icon_back,70,12,48,48,Back,0xede7fb);
}
static void switchRow(lv_obj_t *panel,int y,const lv_img_dsc_t *icon,bool on,int id) {
  auto img=lv_img_create(panel);lv_img_set_src(img,icon);lv_obj_set_pos(img,22,y+15);
  auto sw=lv_switch_create(panel);lv_obj_set_pos(sw,300,y+14);lv_obj_set_size(sw,84,38);
  if(on)lv_obj_add_state(sw,LV_STATE_CHECKED);
  lv_obj_set_style_bg_color(sw,color(0x7fd3b8),LV_PART_INDICATOR|LV_STATE_CHECKED);
  lv_obj_add_event_cb(sw,event,LV_EVENT_CLICKED,reinterpret_cast<void *>(static_cast<intptr_t>(id)));
}
static void showPage(int next) {
  const lv_coord_t keepScroll=page==Settings&&next==Settings&&settingsPanel?lv_obj_get_scroll_y(settingsPanel):0;
  auto oldScreen=screen;
  screen=lv_obj_create(nullptr);
  lv_obj_clear_flag(screen,LV_OBJ_FLAG_SCROLLABLE);lv_obj_set_style_bg_color(screen,color(0xf3efe8),0);
  lv_scr_load(screen);
  if(oldScreen)lv_obj_del(oldScreen);
  page=next;character=background=fallback=fallbackBody=clockLabel=infoLabel=bubble=bubbleLabel=settingsPanel=nullptr;
  toast=nullptr;for(auto &p:stats)p=nullptr;
  bubbleGame=false;stopSound();action=pet.sleeping?4:0;actionAt=millis();lastFrame=UINT32_MAX;
  if(page==Game||page==Ambient) {
    // Animation pauses while the screen is off; show the resting frame so waking never finds an empty image.
    resetWander();
    makeScene();if(pack)drawFrame(clipFor(action).frames[0]);
    topRow(true);
    if(page==Game) {
      const lv_img_dsc_t *icons[]={&icon_stat_food,&icon_stat_mood,&icon_stat_energy};
      const uint32_t tint[]={0xffb347,0xff7a93,0xffd23f};
      for(int i=0;i<3;i++) {
        auto chip=box(screen,74+i*114,360,104,26,0xfffaf3,LV_RADIUS_CIRCLE);lv_obj_set_style_bg_opa(chip,LV_OPA_80,0);
        auto img=lv_img_create(chip);lv_img_set_src(img,icons[i]);lv_obj_set_pos(img,6,3);
        stats[i]=lv_bar_create(chip);lv_obj_set_pos(stats[i],32,8);lv_obj_set_size(stats[i],62,10);
        lv_bar_set_range(stats[i],0,100);lv_obj_clear_flag(stats[i],LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(stats[i],color(0xe9e2d6),LV_PART_MAIN);
        lv_obj_set_style_bg_color(stats[i],color(tint[i]),LV_PART_INDICATOR);
      }
      const lv_img_dsc_t *icons2[]={&icon_act_feed,&icon_act_play,&icon_act_room,&icon_nav_home};
      const int ids[]={GoFoods,GoToys,GoRooms,GoAmbient};
      const uint32_t tints[]={0xffe2bd,0xc0e5e2,0xded6f5,0xf7f3ed};
      for(int i=0;i<4;i++) iconButton(screen,icons2[i],74+i*84,396,76,52,ids[i],tints[i]);
    } else {
      clockLabel=label(screen,"--:--",110,398,220);
      lv_obj_set_style_text_font(clockLabel,&lv_font_montserrat_48,0);
      lv_obj_set_style_text_align(clockLabel,LV_TEXT_ALIGN_CENTER,0);
      iconButton(screen,&icon_nav_game,346,400,64,48,GoGame,0xf7f3ed);
    }
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
    for(int i=0;i<2;i++) {
      auto img=lv_img_create(panel);lv_img_set_src(img,rowIcons[i]);lv_obj_set_pos(img,22,i*66+15);
      iconButton(panel,&icon_minus,224,i*66+9,76,48,i?VolumeDown:BrightnessDown,0xf3efe8);
      iconButton(panel,&icon_plus,312,i*66+9,76,48,i?VolumeUp:BrightnessUp,0xf3efe8);
    }
    switchRow(panel,132,&icon_set_autooff,pet.autoOff,AutoOff);
    switchRow(panel,198,&icon_set_night,pet.nightMute,NightMute);
    auto row=[&](int y,const lv_img_dsc_t *icon) {auto img=lv_img_create(panel);lv_img_set_src(img,icon);lv_obj_set_pos(img,22,y+15);};
    row(264,&icon_set_clock);button(panel,pet.hour24?"24H":"12H",276,273,120,48,Hour24,0xf3efe8);
    row(330,&icon_set_start);
    iconButton(panel,pet.startMode==game::Mode::Game?&icon_set_game:&icon_set_home,276,339,120,48,StartMode,0xf3efe8);
    char tmp[16];snprintf(tmp,sizeof(tmp),"%uH",pet.offlineHours);
    row(396,&icon_set_offline);button(panel,tmp,276,405,120,48,OfflineCap,0xf3efe8);
    const lv_img_dsc_t *actions[]={&icon_set_clock,&icon_set_sync,&icon_set_reload,&icon_set_info};
    const int ids[]={EditTime,Sync,Reload,GoStatus};
    for(int i=0;i<4;i++) iconButton(panel,actions[i],20+i*100,474,88,56,ids[i],0xede7fb);
    label(panel,chargeConfigured?"充電 4.1V / 300mA":"充電停用",24,548,384,0x7a6e7f);
    if(saveLocked) button(panel,"資料損壞：確認重設",20,590,392,60,ResetConfirm,0xffc7c7);
    box(panel,0,saveLocked?650:590,10,20,0xfffaf3,0);  // bottom margin when scrolled to the end
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
static void refuse(game::Result result) {
  if((result==game::Result::Full||result==game::Result::Tired)&&!pet.sleeping) {showPage(Game);setAction(6);}
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
static void syncTime() {
  if(networkBusy) {notify("連接網絡中");return;}
  WiFi.mode(WIFI_STA);WiFi.begin();networkBusy=true;networkAt=millis();notify("連接網絡中");
}
static void control(int id) {
  if(transfer.active) {notify("USB素材傳輸中");return;}
  if(id>=Feed0&&id<Feed0+6) {
    if(!canAct()) return;advanceNow();
    const auto result=game::feed(pet,id-Feed0);
    if(result!=game::Result::Ok) {refuse(result);return;}
    changed();showPage(Game);setAction(2);return;
  }
  if(id>=Play0&&id<Play0+2) {
    if(!canAct()) return;
    if(pet.sleeping) {notify("請先起床");return;}
    if(pet.energy<800) {refuse(game::Result::Tired);return;}
    if(id==Play0) {
      advanceNow();const auto result=game::play(pet,0);
      if(result!=game::Result::Ok){refuse(result);return;}
      changed();showPage(Game);setAction(3);
    } else {
      showPage(Toys);bubbleGame=true;bubbleScore=0;bubbleAt=millis();
      lv_obj_clean(screen);toast=nullptr;topRow(false);
      bubbleLabel=label(screen,"0 / 5",160,30,160);lv_obj_set_style_text_align(bubbleLabel,LV_TEXT_ALIGN_CENTER,0);
      bubble=iconButton(screen,&icon_bubble,185,185,110,110,Bubble,0xdff4ff);
    }
    return;
  }
  if(id>=Room0&&id<Room0+int(kMaxBackgrounds)) {
    if(!canAct()) return;
    pet.background=id-Room0;changed();showPage(Game);requestAssets();setAction(5);return;
  }
  switch(id) {
    case GoGame:showPage(Game);break;
    case GoAmbient:showPage(Ambient);break;
    case GoSettings:showPage(Settings);break;
    case GoFoods:if(canAct())showPage(Foods);break;
    case GoToys:if(canAct())showPage(Toys);break;
    case GoRooms:if(canAct())showPage(Rooms);break;
    case GoStatus:showPage(Status);break;
    case Tap:
      if(page<=Ambient&&canAct()) {
        // A random reaction, never the same one twice in a row.
        if(pet.sleeping) setAction(4);
        else if(pack&&pack->tapCount) {
          int i=esp_random()%pack->tapCount;
          if(i==lastTap&&pack->tapCount>1) i=(i+1)%pack->tapCount;
          lastTap=i;setAction(ActTap0+i);
        } else setAction(pet.bond>=40?7:1);
      }
      break;
    case Back:showPage(page==Status||page==SetTime?Settings:Game);break;
    case Sleep:
      if(!canAct())break;advanceNow();pet.sleeping=!pet.sleeping;pet.sleepSeconds=0;changed();
      showPage(page);break;
    case BrightnessDown:pet.brightness=std::max<int>(20,pet.brightness-20);changed();display.setBrightness(pet.brightness);break;
    case BrightnessUp:pet.brightness=std::min<int>(220,pet.brightness+20);changed();display.setBrightness(pet.brightness);break;
    case VolumeDown:pet.volume=std::max<int>(0,pet.volume-10);changed();break;
    case VolumeUp:pet.volume=std::min<int>(100,pet.volume+10);changed();soundFor(1);break;
    // Switches show their own state, so these two do not rebuild the page.
    case AutoOff:pet.autoOff=!pet.autoOff;changed();break;
    case Hour24:pet.hour24=!pet.hour24;changed();showPage(Settings);break;
    case NightMute:pet.nightMute=!pet.nightMute;changed();break;
    case StartMode:
      pet.startMode=pet.startMode==game::Mode::Game?game::Mode::Ambient:game::Mode::Game;changed();showPage(Settings);break;
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
    case Sync:syncTime();break;
    case Bubble:
      if(!bubbleGame)break;
      if(++bubbleScore>=5) {
        advanceNow();const auto result=game::play(pet,1);bubbleGame=false;
        if(result==game::Result::Ok) {changed();showPage(Game);setAction(7);}
        else {showPage(Game);refuse(result);}
      } else {
        char score[16];snprintf(score,sizeof(score),"%u / 5",bubbleScore);lv_label_set_text(bubbleLabel,score);
        lv_obj_set_pos(bubble,70+esp_random()%230,110+esp_random()%220);
      }
      break;
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
  if((page!=Game&&page!=Ambient)||!displayOn||transfer.active) return;
  const uint32_t delta=millis()-actionAt;
  if(pack&&character) {
    wanderTick();
    const Clip &c=clipFor(action);uint32_t total=0;for(unsigned i=0;i<c.count;i++)total+=c.durations[i];
    if(!total)return;
    if(!c.loop&&delta>=total) {setAction(pet.sleeping?4:0);return;}
    uint32_t phase=delta%total;unsigned index=0;
    while(index+1<c.count&&phase>=c.durations[index]) {phase-=c.durations[index];index++;}
    drawFrame(c.frames[index]);
    const bool walking=wander==Wander::Walk&&resting(action)&&!pet.sleeping;
    const bool lively=action==3||action==7||(action>=ActTap0&&action<ActWalk);
    // Walking hops a little on each step; sitting and sleeping stay still.
    const int bounce=walking?-int(std::fabs(std::sin(millis()/170.0))*7):
      action==ActSit||action==4?0:int(std::sin(delta/220.0)*(lively?8:2));
    const bool home=pet.sleeping;  // asleep: stays at the centre spot
    lv_obj_set_pos(character,home?restX():int(wanderX),(home?homeY():int(wanderY))+bounce);
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
  }
  if(page==Game) {
    const int values[]={pet.hunger,pet.mood,pet.energy};
    for(int i=0;i<3;i++)if(stats[i])lv_bar_set_value(stats[i],values[i]/100,LV_ANIM_OFF);
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
    reply["battery_sample_ms"]=batteryReadAt;reply["usb_power"]=usbPower;reply["charging"]=charging;
    reply["utc"]=nowUtc();int64_t rtc=0;reply["rtc_valid"]=rtcRead(rtc);reply["rtc_utc"]=rtc;
    reply["save_generation"]=pet.generation;reply["save_locked"]=saveLocked;reply["dirty"]=dirty;
    reply["background"]=pet.background;reply["background_count"]=pack?pack->backgroundCount:0;
    reply["hunger"]=pet.hunger;reply["mood"]=pet.mood;reply["energy"]=pet.energy;
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
       (value>=Play0&&value<Play0+2)||(value>=Room0&&value<Room0+int(kMaxBackgrounds))||(value>=BrightnessDown&&value<=ResetConfirm)))
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
  if(pet.startMode==game::Mode::Clock) pet.startMode=game::Mode::Ambient;  // clock mode was removed
  lastTickUs=esp_timer_get_time();lastInput=millis();
  Serial.println("INIT SD");
  assets.begin();showPage(pet.startMode==game::Mode::Game?Game:Ambient);
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
  AssetResult loaded;
  if(assets.poll(loaded)) {
    assetLoading=false;
    if(loaded.generation!=sceneGeneration)delete loaded.pack;
    else if(!loaded.pack) {
      if(pack&&pet.background!=pack->selected){pet.background=pack->selected;changed();}
      showPage(page);notify(loaded.error,10000);
    }
    else {
      stopSound();
      lv_img_cache_invalidate_src(nullptr);
      auto oldPack=std::move(pack);
      pack.reset(loaded.pack);
      if(pet.background!=pack->selected){pet.background=pack->selected;changed();}  // pack has fewer rooms
      showPage(page);
    }
  }
  if(!capture&&millis()-lastFrameAt>=66) {lastFrameAt=millis();updateAnimation();}
  static uint32_t lastLabels=0;
  if(millis()-lastLabels>=250){lastLabels=millis();updateLabels();}
  if(dirty&&!saveLocked&&!transfer.active&&millis()-dirtyAt>=2000&&
     (urgentSave||millis()-lastSave>=60000||pet.generation<2)) saveNow();
  if(bubbleGame&&millis()-bubbleAt>30000)showPage(Game);
  if(page==Game&&millis()-lastInput>45000&&!transfer.active)showPage(Ambient);
  if(capture&&millis()-captureAt>15000)capture=false;
  if(!capture)lv_timer_handler();
  if(audioFailures.load()!=audioFailureSeen) {
    audioFailureSeen=audioFailures.load();notify("音效播放失敗");
  }
  maxLoopUs=std::max<uint32_t>(maxLoopUs,esp_timer_get_time()-start);
  delay(3);
}
