#include "Games.h"
#include <Preferences.h>
#include <esp_random.h>
#include <algorithm>
#include <cmath>
#include "icons.h"

namespace games {
namespace {
Host host;
Id current=Catch;
bool active=false,over=false;
uint32_t lastTick=0,startedAt=0,hintUntil=0,poseUntil=0;
unsigned score=0;
float dt=0;

lv_obj_t *pet=nullptr,*scoreLabel=nullptr,*hint=nullptr;
lv_img_dsc_t petImg{};
int petFrame=-1,zoom=256;
float petX=240,petFeet=458;
bool pressed=false,tapped=false;
lv_point_t touch{},tapAt{};

// Frames for the poses the games use, taken from the pack's clips (fallbacks keep demo packs working).
struct Poses {int stand,blink,walkA,walkB,jump,happy,cheer,wave,look,eat,satisfied,refuse;} P;
int frameOf(const Clip &c,unsigned i,int fallback) {return c.count>i?c.frames[i]:fallback;}
void loadPoses() {
  const auto &p=*host.pack;using namespace behavior;
  P.stand=frameOf(p.clips[0],0,0);P.blink=frameOf(p.clips[0],1,P.stand);
  P.walkA=frameOf(p.walk,0,P.stand);P.walkB=frameOf(p.walk,1,P.walkA);
  P.happy=frameOf(p.gestures[Smile],0,P.stand);P.jump=frameOf(p.gestures[Hop],0,P.happy);
  P.cheer=frameOf(p.gestures[Stretch],0,P.happy);P.wave=frameOf(p.gestures[Wave],0,P.happy);
  P.look=frameOf(p.gestures[Look],0,P.stand);P.eat=frameOf(p.clips[2],0,P.happy);
  P.satisfied=frameOf(p.clips[2],1,P.happy);P.refuse=frameOf(p.clips[6],0,P.look);
}
int rnd(int n) {return int(esp_random()%unsigned(n));}
int frameSize() {return host.pack->atlasHeader.width;}
float petHeight() {return 0.855f*frameSize()*zoom/256;}  // visible standing height
void showFrame(int f) {
  if(f==petFrame) return;
  const auto &h=host.pack->atlasHeader;
  petImg={};petImg.header.cf=LV_IMG_CF_TRUE_COLOR_ALPHA;petImg.header.w=h.width;petImg.header.h=h.height;
  petImg.data_size=h.width*h.height*3;petImg.data=host.pack->atlas->data+16+f*petImg.data_size;
  lv_img_cache_invalidate_src(&petImg);lv_img_set_src(pet,&petImg);petFrame=f;
}
// Holds a pose for a moment; each game puts its resting pose back once it has passed.
void pose(int f,uint32_t ms) {showFrame(f);poseUntil=millis()+ms;}
bool posing() {return int32_t(poseUntil-millis())>0;}
// LVGL zooms around the frame centre: position from the visible centre x and feet y.
void place() {
  const int s=frameSize(),h=s/2;
  lv_obj_set_pos(pet,int(petX)-h,int(petFeet)-h-(s-4-h)*zoom/256);
}
void makePet(int z,float x,float feet) {
  zoom=z;petX=x;petFeet=feet;petFrame=-1;
  pet=lv_img_create(host.screen);if(zoom!=256) lv_img_set_zoom(pet,zoom);
  lv_obj_clear_flag(pet,LV_OBJ_FLAG_CLICKABLE);showFrame(P.stand);place();
}
lv_obj_t *box(lv_obj_t *parent,int x,int y,int w,int h,uint32_t bg,int radius) {
  auto o=lv_obj_create(parent);lv_obj_set_pos(o,x,y);lv_obj_set_size(o,w,h);
  lv_obj_set_style_bg_color(o,lv_color_hex(bg),0);lv_obj_set_style_radius(o,radius,0);
  lv_obj_set_style_border_width(o,0,0);lv_obj_set_style_pad_all(o,0,0);
  lv_obj_clear_flag(o,LV_OBJ_FLAG_SCROLLABLE);lv_obj_clear_flag(o,LV_OBJ_FLAG_CLICKABLE);
  return o;
}
lv_obj_t *text(lv_obj_t *parent,const char *s,int x,int y,int w,uint32_t ink=0x4a3f4f) {
  auto l=lv_label_create(parent);lv_label_set_text(l,s);lv_obj_set_pos(l,x,y);lv_obj_set_width(l,w);
  lv_obj_set_style_text_font(l,host.font,0);lv_obj_set_style_text_color(l,lv_color_hex(ink),0);
  lv_obj_set_style_text_align(l,LV_TEXT_ALIGN_CENTER,0);
  return l;
}
lv_obj_t *icon(const lv_img_dsc_t *src,float cx,float cy) {
  auto i=lv_img_create(host.screen);lv_img_set_src(i,src);lv_obj_clear_flag(i,LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_pos(i,int(cx)-src->header.w/2,int(cy)-src->header.h/2);
  return i;
}
void moveIcon(lv_obj_t *o,float cx,float cy) {
  lv_obj_set_pos(o,int(cx)-lv_obj_get_self_width(o)/2,int(cy)-lv_obj_get_self_height(o)/2);
}
void sound(int slot) {if(host.sound) host.sound(slot);}
void say(const char *s,uint32_t ms=2500) {
  if(!hint) return;  // during begin(): start() shows the opening line once the label exists
  lv_label_set_text(hint,s);lv_obj_clear_flag(hint,LV_OBJ_FLAG_HIDDEN);hintUntil=millis()+ms;
}
void setScore(const char *s) {if(strcmp(lv_label_get_text(scoreLabel),s)) lv_label_set_text(scoreLabel,s);}

void onTouch(lv_event_t *e) {
  lv_indev_t *in=lv_indev_get_act();if(!in) return;
  lv_indev_get_point(in,&touch);
  const auto code=lv_event_get_code(e);
  if(code==LV_EVENT_PRESSED) {pressed=true;tapped=true;tapAt=touch;}
  else if(code==LV_EVENT_RELEASED||code==LV_EVENT_PRESS_LOST) pressed=false;
}
lv_obj_t *button(lv_obj_t *parent,const char *label,const lv_img_dsc_t *img,int x,int y,int w,int h,uint32_t bg,
                 lv_event_cb_t cb,intptr_t data) {
  auto b=lv_btn_create(parent);lv_obj_set_pos(b,x,y);lv_obj_set_size(b,w,h);
  lv_obj_set_style_radius(b,LV_RADIUS_CIRCLE,0);lv_obj_set_style_bg_color(b,lv_color_hex(bg),0);
  lv_obj_set_style_shadow_width(b,0,0);lv_obj_set_style_border_width(b,0,0);lv_obj_set_style_pad_all(b,0,0);
  lv_obj_add_event_cb(b,cb,LV_EVENT_CLICKED,reinterpret_cast<void *>(data));
  if(img) {auto i=lv_img_create(b);lv_img_set_src(i,img);lv_obj_center(i);}
  else {auto l=text(b,label,0,0,w);lv_obj_center(l);}
  return b;
}
void onExit(lv_event_t *) {if(host.exit) host.exit();}
void onRestart(lv_event_t *) {if(host.restart) host.restart();}

void finish() {
  if(over) return;
  over=true;
  const unsigned before=best(current);const bool record=score>before;
  if(record) {
    Preferences p;
    if(p.begin("cggames",false)) {char k[4];snprintf(k,sizeof(k),"b%u",unsigned(current));p.putUInt(k,score);p.end();}
  }
  if(host.reward) host.reward(score);
  sound(record?1:0);
  auto panel=box(host.screen,60,104,360,264,0xfffaf3,36);lv_obj_set_style_bg_opa(panel,LV_OPA_90,0);
  lv_obj_add_flag(panel,LV_OBJ_FLAG_CLICKABLE);
  text(panel,record?"新紀錄！":"完！",0,22,360);
  char b[40];snprintf(b,sizeof(b),"分數 %u",score);text(panel,b,0,74,360);
  snprintf(b,sizeof(b),"最高 %u",record?score:before);text(panel,b,0,110,360,0x7a6e7f);
  button(panel,"再玩",nullptr,34,170,136,60,0xffd23f,onRestart,0);
  button(panel,"返回",nullptr,190,170,136,60,0xede7fb,onExit,0);
}

// --- 接零食: walk under the falling snacks, dodge the broccoli. 30 seconds.
namespace catchgame {
struct Item {lv_obj_t *o;float x,y,vy;bool bad;};
Item items[8];unsigned count=0;
uint32_t nextSpawn=0,stepAt=0;float target=240;bool step=false;
const lv_img_dsc_t *foods[]={&icon_m_food0,&icon_m_food1,&icon_m_food2,&icon_m_food3,&icon_m_food4,&icon_m_food5};
void begin() {
  count=0;target=240;makePet(154,240,458);nextSpawn=millis()+1500;
  say("撳住畫面左右移動 CHIMMY");
}
void update(uint32_t now) {
  const uint32_t elapsed=now-startedAt;
  const int left=30-int(elapsed/1000);
  if(left<=0) {finish();return;}
  if(pressed) target=std::clamp<float>(touch.x,80,400);
  const float d=target-petX,move=320*dt;const bool walking=std::fabs(d)>4;
  if(walking) petX+=d>0?std::min(move,d):std::max(-move,d);
  if(!posing()) {
    if(walking) {if(now-stepAt>170) {stepAt=now;step=!step;}showFrame(step?P.walkA:P.walkB);}
    else showFrame(P.stand);
  }
  place();
  if(int32_t(now-nextSpawn)>=0&&count<8) {
    auto &it=items[count++];
    it.bad=rnd(100)<22;it.x=60+rnd(361);it.y=-30;it.vy=140+elapsed/1000.0f*5+rnd(40);
    it.o=icon(it.bad?&icon_g_broccoli:foods[rnd(6)],it.x,it.y);
    nextSpawn=now+std::max<uint32_t>(420,900-elapsed/50);
  }
  const float head=petFeet-petHeight();
  for(unsigned i=0;i<count;) {
    auto &it=items[i];it.y+=it.vy*dt;moveIcon(it.o,it.x,it.y);
    const bool caught=it.y>head&&it.y<head+90&&std::fabs(it.x-petX)<64;
    if(caught&&it.bad) {score=score?score-1:0;pose(P.refuse,600);sound(0);}
    else if(caught) {score++;pose(P.eat,350);sound(2);}
    if(caught||it.y>510) {lv_obj_del(it.o);items[i]=items[--count];continue;}
    i++;
  }
  char b[40];snprintf(b,sizeof(b),"分數 %u · %d秒",score,left);setScore(b);
}
}

// --- 頂波波: tap when the ball drops onto CHIMMY's head; it keeps getting faster.
namespace lift {
lv_obj_t *ball=nullptr;float bx,by,vx,vy;uint32_t stepAt=0;bool step=false;
void begin() {
  makePet(154,240,458);bx=240;by=96;vx=vy=0;ball=icon(&icon_g_ball,bx,by);
  say("個波跌到 CHIMMY 頭頂就撳！",3000);
}
void update(uint32_t now) {
  const float head=petFeet-petHeight();
  if(now-startedAt>1500) {
    vy+=std::min(1300.0f,700.0f+score*25)*dt;bx+=vx*dt;by+=vy*dt;
    if(bx<50||bx>430) {vx=-vx;bx=std::clamp<float>(bx,50,430);}
    if(tapped&&vy>0&&by>head-120&&by<head+10) {
      vy=-(560.0f+rnd(120));vx=float(rnd(240)-120);score++;pose(P.jump,260);sound(0);
    }
    if(by>head+24) {pose(P.refuse,2000);finish();return;}
  }
  moveIcon(ball,bx,by);
  const float d=bx-petX,move=330*dt;const bool walking=std::fabs(d)>6;
  if(walking) petX+=d>0?std::min(move,d):std::max(-move,d);
  petX=std::clamp<float>(petX,80,400);
  if(!posing()) {
    if(walking) {if(now-stepAt>170) {stepAt=now;step=!step;}showFrame(step?P.walkA:P.walkB);}
    else showFrame(P.look);
  }
  place();
  char b[24];snprintf(b,sizeof(b),"%u 下",score);setScore(b);
}
}

// --- 估下喺邊: CHIMMY hides under one of three cushions, they swap, find him. Three lives.
namespace hide {
enum Phase {Show,Cover,Shuffle,Guess,Reveal};
Phase phase=Show;uint32_t at=0,swapMs=500;
lv_obj_t *cushion[3]{},*lives[3]{};
float cx[3];int home=0;unsigned left=3,round=0,swapsLeft=0;
int a=0,b=1;float ax=0,bxs=0;
constexpr float slotX[3]={120,240,360},cushionY=344;
void startRound(uint32_t now) {
  for(int i=0;i<3;i++) {cx[i]=slotX[i];moveIcon(cushion[i],cx[i],cushionY);}
  home=rnd(3);petX=cx[home];place();lv_obj_clear_flag(pet,LV_OBJ_FLAG_HIDDEN);showFrame(P.wave);
  say("記住 CHIMMY 喺邊！",1300);phase=Show;at=now;
}
void nextSwap(uint32_t now) {
  a=rnd(3);b=(a+1+rnd(2))%3;ax=cx[a];bxs=cx[b];at=now;
}
void begin() {
  left=3;round=0;
  makePet(120,240,372);  // created first: the cushions sit in front of him
  for(int i=0;i<3;i++) cushion[i]=icon(&icon_g_cushion,slotX[i],cushionY);
  for(int i=0;i<3;i++) lives[i]=icon(&icon_g_life,206+i*34,124);
  startRound(millis());
}
void update(uint32_t now) {
  switch(phase) {
    case Show:
      if(now-at>1300) {lv_obj_add_flag(pet,LV_OBJ_FLAG_HIDDEN);phase=Cover;at=now;}
      break;
    case Cover:
      if(now-at>400) {swapsLeft=3+round;swapMs=std::max<uint32_t>(240,560-round*40);phase=Shuffle;nextSwap(now);}
      break;
    case Shuffle: {
      // The two cushions trade places along little arcs (one over, one under).
      const float t=std::min(1.0f,float(now-at)/swapMs),arc=std::sin(t*3.14159f)*36;
      cx[a]=ax+(bxs-ax)*t;cx[b]=bxs+(ax-bxs)*t;
      moveIcon(cushion[a],cx[a],cushionY-arc);moveIcon(cushion[b],cx[b],cushionY+arc*0.5f);
      if(t>=1) {
        moveIcon(cushion[a],cx[a],cushionY);moveIcon(cushion[b],cx[b],cushionY);
        if(--swapsLeft) nextSwap(now); else {phase=Guess;say("CHIMMY 喺邊？",4000);}
      }
      break;
    }
    case Guess:
      if(!tapped) break;
      for(int i=0;i<3;i++) if(std::fabs(tapAt.x-cx[i])<64&&std::fabs(tapAt.y-cushionY)<80) {
        petX=cx[home];place();lv_obj_clear_flag(pet,LV_OBJ_FLAG_HIDDEN);
        if(i==home) {score++;showFrame(P.cheer);sound(1);say("搵到喇！",1200);}
        else {left--;lv_obj_add_flag(lives[left],LV_OBJ_FLAG_HIDDEN);showFrame(P.look);sound(0);say("喺呢度呀～",1200);}
        phase=Reveal;at=now;break;
      }
      break;
    case Reveal:
      if(now-at>1400) {if(!left) finish(); else {round++;startRound(now);}}
      break;
  }
  char s[24];snprintf(s,sizeof(s),"分數 %u",score);setScore(s);
}
}

// --- 跟住跳: CHIMMY shows a dance; press the moves back in the same order. One step longer each round.
namespace mimic {
const lv_img_dsc_t *const icons[4]={&icon_g_wave,&icon_g_cheer,&icon_g_hop,&icon_g_look};
const uint32_t tints[4]={0xffd6e0,0xfff1b8,0xc9f0e2,0xffe2c4};
enum Phase {Watch,Input,Next,Wrong};
Phase phase=Watch;uint32_t at=0;
lv_obj_t *buttons[4]{};uint8_t seq[40];unsigned len=1,pos=0;int pending=-1;bool lit=false;
int poseFor(int m) {const int f[4]={P.wave,P.cheer,P.jump,P.look};return f[m];}
void light(int m,bool on) {
  lv_obj_set_style_border_width(buttons[m],on?5:0,0);lv_obj_set_style_border_color(buttons[m],lv_color_hex(0x4a3f4f),0);
}
void onMove(lv_event_t *e) {pending=int(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));}
void watch(uint32_t now) {phase=Watch;pos=0;at=now;lit=false;say("睇住 CHIMMY！",1200);}
void begin() {
  len=1;seq[0]=rnd(4);pending=-1;
  makePet(200,240,348);
  for(int i=0;i<4;i++) buttons[i]=button(host.screen,nullptr,icons[i],54+i*96,368,84,84,tints[i],onMove,i);
  watch(millis()+600);
}
void update(uint32_t now) {
  const uint32_t stepMs=std::max<uint32_t>(380,650-len*15);
  switch(phase) {
    case Watch: {
      if(int32_t(now-at)<0) break;
      // Each move: lit button + pose + its own tone, then a short gap.
      const unsigned k=(now-at)/(stepMs+220),inStep=(now-at)%(stepMs+220);
      if(k>=len) {for(int i=0;i<4;i++) light(i,false);showFrame(P.stand);phase=Input;pending=-1;say("到你！",1500);break;}
      const bool on=inStep<stepMs;
      if(on&&!lit) {light(seq[k],true);showFrame(poseFor(seq[k]));sound(seq[k]);lit=true;}
      if(!on&&lit) {light(seq[k],false);showFrame(P.stand);lit=false;}
      break;
    }
    case Input:
      if(pending<0) {if(!posing()) showFrame(P.stand);break;}
      pose(poseFor(pending),300);sound(pending);
      if(pending!=seq[pos]) {pose(P.refuse,1500);say("唔啱呀～",1500);phase=Wrong;at=now;}
      else if(++pos==len) {score=len;phase=Next;at=now;}
      pending=-1;
      break;
    case Next:
      if(now-at>400&&now-at<1100) {showFrame(P.happy);}
      if(now-at>1100) {if(len<sizeof(seq)) seq[len++]=rnd(4);say("好叻！再嚟",900);watch(now+300);}
      break;
    case Wrong:
      if(now-at>1200) finish();
      break;
  }
  char s[24];snprintf(s,sizeof(s),"分數 %u",score);setScore(s);
}
}
}  // namespace

void start(Id id,const Host &h) {
  host=h;current=id;active=true;over=false;score=0;pressed=tapped=false;poseUntil=0;
  loadPoses();
  // Full-screen touch pad underneath everything else.
  auto pad=lv_obj_create(host.screen);lv_obj_set_pos(pad,0,0);lv_obj_set_size(pad,480,480);
  lv_obj_set_style_bg_opa(pad,LV_OPA_TRANSP,0);lv_obj_set_style_border_width(pad,0,0);
  lv_obj_clear_flag(pad,LV_OBJ_FLAG_SCROLLABLE);
  for(auto code:{LV_EVENT_PRESSED,LV_EVENT_PRESSING,LV_EVENT_RELEASED,LV_EVENT_PRESS_LOST})
    lv_obj_add_event_cb(pad,onTouch,code,nullptr);
  startedAt=lastTick=millis();
  switch(id) {
    case Catch: catchgame::begin();break;
    case Lift: lift::begin();break;
    case Hide: hide::begin();break;
    case Mimic: mimic::begin();break;
    default: break;
  }
  auto pill=box(host.screen,140,66,200,40,0xfffaf3,20);lv_obj_set_style_bg_opa(pill,LV_OPA_80,0);
  scoreLabel=text(pill,"",0,4,200);
  hint=text(host.screen,"",40,156,400,0x594761);lv_obj_add_flag(hint,LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_style_bg_color(hint,lv_color_hex(0xfff0cf),0);lv_obj_set_style_bg_opa(hint,LV_OPA_90,0);
  lv_obj_set_style_radius(hint,14,0);lv_obj_set_style_pad_all(hint,6,0);
  // Re-show the opening hint now that its label exists.
  if(id==Catch) say("撳住畫面左右移動 CHIMMY");
  if(id==Lift) say("個波跌到 CHIMMY 頭頂就撳！",3000);
  if(id==Hide) say("記住 CHIMMY 喺邊！",1300);
  if(id==Mimic) say("睇住 CHIMMY！",1200);
  button(host.screen,nullptr,&icon_back,70,12,48,48,0xede7fb,onExit,0);
}
void tick() {
  if(!active) return;
  const uint32_t now=millis();
  if(now-lastTick<30) return;
  dt=std::min<uint32_t>(now-lastTick,100)/1000.0f;lastTick=now;
  if(hint&&int32_t(now-hintUntil)>=0) lv_obj_add_flag(hint,LV_OBJ_FLAG_HIDDEN);
  if(!over) switch(current) {
    case Catch: catchgame::update(now);break;
    case Lift: lift::update(now);break;
    case Hide: hide::update(now);break;
    case Mimic: mimic::update(now);break;
    default: break;
  }
  tapped=false;
}
void stop() {active=false;pet=scoreLabel=hint=nullptr;}
bool running() {return active;}
unsigned best(Id id) {
  Preferences p;unsigned v=0;
  if(p.begin("cggames",true)) {char k[4];snprintf(k,sizeof(k),"b%u",unsigned(id));v=p.getUInt(k,0);p.end();}
  return v;
}
}  // namespace games
