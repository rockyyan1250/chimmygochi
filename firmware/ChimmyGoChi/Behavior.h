#pragma once
#include <algorithm>
#include <cstdint>

// CHIMMY's own life when nobody is touching the screen: rest (breathing and blinking only),
// then one short act picked by weighted draw, then rest again. Plain C++ with no LVGL or
// Arduino so tools/behavior-test.cpp can check the rules on the PC.
namespace behavior {
enum Gesture : uint8_t { Look, Smile, Content, Wave, Stretch, Hop, Doze, Ball, kGestures };
constexpr const char *gestureNames[kGestures]={"look","smile","content","wave","stretch","hop","doze","ball"};
enum class Kind : uint8_t { Rest, Walk, Sit, Gesture, Sleep };

struct Context {
  uint32_t now=0;        // ms
  int hour=-1;           // local hour; -1 while the clock is unknown
  uint32_t idleMs=0;     // since the last touch
  uint32_t bond=0;
  bool calm=false;       // bus/clock screens: small gestures only
  bool canWalk=true,hasSit=false;
  uint8_t available=0;   // bit per Gesture that the asset pack provides
  bool asleep=false;
};
struct Step {
  Kind kind=Kind::Rest;
  uint8_t gesture=0;
  uint32_t ms=0;  // Rest/Sit/Doze length; 0 = until the clip or the walk ends
};

constexpr uint32_t kSleepIdleMs=15*60*1000;  // night: fall asleep after 15 quiet minutes
inline bool night(int hour) {return hour>=23||(hour>=0&&hour<7);}
inline bool drowsy(int hour) {return night(hour)||(hour>=13&&hour<15)||hour>=21;}
constexpr bool big(Gesture g) {return g==Stretch||g==Hop||g==Ball||g==Wave;}

class Engine {
 public:
  explicit Engine(uint32_t seed=1):rng(seed?seed:1) {}
  void interrupt() {afterUser=true;}  // a touch reaction just finished
  void woke(uint32_t now) {wokeAt=now;woken=true;}
  Step next(const Context &c) {
    // After any act (or a touch) comes a quiet spell; acts never follow each other directly.
    if(last!=Kind::Rest||afterUser) {
      const bool short_=afterUser;afterUser=false;last=Kind::Rest;
      Step s;s.ms=c.asleep?60000:short_?range(2000,5000):c.calm?range(12000,30000):range(8000,20000);
      return s;
    }
    if(c.asleep) return rest(60000);
    // Night and nobody around: doze off first, then sleep.
    if(night(c.hour)&&c.idleMs>=kSleepIdleMs) {
      if(dozed) {dozed=false;return act(Kind::Sleep,0,0);}
      if(c.available&(1<<Doze)) {dozed=true;return act(Kind::Gesture,Doze,range(15000,25000));}
      return act(Kind::Sleep,0,0);
    }
    dozed=false;
    uint32_t w[kGestures+2]{};  // gestures, then walk, sit (weights x10)
    const bool attentive=c.idleMs<60000;
    const uint32_t base[kGestures]={100,70,60,30+std::min<uint32_t>(60,c.bond*10/40),30,30,20,20};
    for(unsigned g=0;g<kGestures;g++) {
      if(!(c.available&(1<<g))||int32_t(c.now-cooldown[g])<0) continue;
      if(c.calm&&big(Gesture(g))) continue;
      uint32_t x=base[g];
      if(g==Doze) x=drowsy(c.hour)?x*4:x/2;
      if(g==Stretch&&(sat||(woken&&c.now-wokeAt<5*60000))) x*=4;
      if(attentive&&(g==Look||g==Smile||g==Wave)) x*=2;
      for(uint8_t r:recent) if(r==g) x=x*3/10;
      w[g]=x;
    }
    w[kGestures]=c.canWalk?(c.calm?30:60)*(night(c.hour)?1:2)/2:0;
    w[kGestures+1]=c.hasSit?40:0;
    uint32_t total=0;for(uint32_t x:w) total+=x;
    if(!total) return rest(10000);
    uint32_t pick=rand()%total,i=0;
    while(pick>=w[i]) pick-=w[i++];
    sat=false;
    if(i==kGestures) return act(Kind::Walk,0,0);
    if(i==kGestures+1) {sat=true;return act(Kind::Sit,0,range(15000,45000));}
    remember(Gesture(i),c.now);
    return act(Kind::Gesture,uint8_t(i),i==Doze?range(15000,40000):0);
  }
  static constexpr uint32_t cooldownMs[kGestures]={40000,60000,90000,90000,180000,150000,120000,300000};

 private:
  uint32_t rng,cooldown[kGestures]{},wokeAt=0;
  uint8_t recent[4]={255,255,255,255};
  Kind last=Kind::Rest;
  bool afterUser=false,sat=false,woken=false,dozed=false;
  uint32_t rand() {rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;return rng;}
  uint32_t range(uint32_t lo,uint32_t hi) {return lo+rand()%(hi-lo+1);}
  Step rest(uint32_t ms) {last=Kind::Rest;Step s;s.ms=ms;return s;}
  Step act(Kind k,uint8_t g,uint32_t ms) {last=k;Step s;s.kind=k;s.gesture=g;s.ms=ms;return s;}
  void remember(Gesture g,uint32_t now) {
    cooldown[g]=now+cooldownMs[g];
    std::copy_backward(recent,recent+3,recent+4);recent[0]=g;
  }
};
}  // namespace behavior
