#include "../firmware/ChimmyGoChi/Behavior.h"
#include <cassert>
#include <cstdio>

using namespace behavior;

// Simulates `hours` of CHIMMY's day: every step ends after its length (gestures ~2s, walks ~6s).
static void simulate(Context c,unsigned hours,uint32_t seed,bool interruptEvery10=false) {
  Engine e(seed);
  uint32_t lastAct[kGestures];for(auto &x:lastAct) x=UINT32_MAX;
  bool prevWasAct=false;  // the page itself rests briefly before asking for the first step
  unsigned steps=0;
  for(c.now=1;c.now<hours*3600000u;steps++) {
    if(interruptEvery10&&steps%10==9) e.interrupt();
    const Step s=e.next(c);
    if(s.kind==Kind::Rest) {
      assert(s.ms>=2000);
      if(!c.asleep&&!interruptEvery10) assert(c.calm?(s.ms>=12000&&s.ms<=30000):(s.ms>=8000&&s.ms<=20000));
      prevWasAct=false;
    } else {
      assert(!prevWasAct);  // never two acts in a row
      assert(!c.asleep);
      prevWasAct=true;
      if(s.kind==Kind::Gesture) {
        assert(c.available&(1<<s.gesture));
        if(c.calm) assert(!big(Gesture(s.gesture)));
        if(lastAct[s.gesture]!=UINT32_MAX) assert(c.now-lastAct[s.gesture]>=Engine::cooldownMs[s.gesture]);
        lastAct[s.gesture]=c.now;
      }
      if(s.kind==Kind::Walk) assert(c.canWalk);
      if(s.kind==Kind::Sit) assert(c.hasSit&&s.ms>=15000&&s.ms<=45000);
    }
    c.now+=s.ms?s.ms:s.kind==Kind::Walk?6000:2000;
  }
}

int main() {
  Context day;day.hour=15;day.idleMs=10*60000;day.hasSit=true;day.available=0xff;day.bond=80;
  for(uint32_t seed=1;seed<40;seed++) simulate(day,6,seed);
  for(uint32_t seed=1;seed<10;seed++) simulate(day,2,seed,true);  // touches cut in: short rests allowed

  Context calm=day;calm.calm=true;
  for(uint32_t seed=1;seed<20;seed++) simulate(calm,6,seed);

  Context few=day;few.available=(1<<Look)|(1<<Smile);few.hasSit=false;few.canWalk=false;
  for(uint32_t seed=1;seed<10;seed++) simulate(few,2,seed);

  Context none=few;none.available=0;
  {Engine e(3);e.next(none);const Step s=e.next(none);assert(s.kind==Kind::Rest);}

  Context asleep=day;asleep.asleep=true;
  for(uint32_t seed=1;seed<5;seed++) simulate(asleep,2,seed);

  // Night, nobody around for 15 minutes: doze, then sleep (with a rest in between).
  Context late=day;late.hour=23;late.idleMs=kSleepIdleMs;
  {
    Engine e(7);
    const Step doze=e.next(late);assert(doze.kind==Kind::Gesture&&doze.gesture==Doze&&doze.ms>=15000);
    assert(e.next(late).kind==Kind::Rest);
    assert(e.next(late).kind==Kind::Sleep);
  }
  // ...but not while someone is still around.
  {
    Context busy=late;busy.idleMs=60000;Engine e(7);
    for(int i=0;i<200;i++) assert(e.next(busy).kind!=Kind::Sleep);
  }
  // A touch makes the next quiet spell short.
  {
    Engine e(5);e.next(day);e.next(day);e.interrupt();
    const Step s=e.next(day);assert(s.kind==Kind::Rest&&s.ms>=2000&&s.ms<=5000);
  }
  // Variety: over a day every available gesture appears.
  {
    Engine e(11);Context c=day;bool seen[kGestures]{};
    for(c.now=1;c.now<12*3600000u;) {const Step s=e.next(c);if(s.kind==Kind::Gesture)seen[s.gesture]=true;c.now+=s.ms?s.ms:3000;}
    for(bool b:seen) assert(b);
  }
  puts("Behavior engine rules hold.");
}
