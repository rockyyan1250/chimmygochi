#include "../firmware/ChimmyGoChi/Battery.h"
#include <cassert>
#include <cstring>

int main() {
  using battery::State;
  struct Case {
    int detection,status,gauge,percent;
    State expected;
    int expectedPercent;
    const char *text;
    const char *name;
  };
  const Case cases[]={
    {1,8,8,0,State::Ready,0,"0%","ready"},
    {1,8,8,20,State::Ready,20,"20%","ready"},
    {1,8,8,100,State::Ready,100,"100%","ready"},
    {1,0,-1,-1,State::Disconnected,-1,"--","disconnected"},
    {-1,8,8,50,State::Unavailable,-1,"ERR","unavailable"},
    {0,8,8,50,State::Unavailable,-1,"ERR","unavailable"},
    {1,-1,8,50,State::Unavailable,-1,"ERR","unavailable"},
    {1,8,-1,50,State::Unavailable,-1,"ERR","unavailable"},
    {1,8,0,50,State::Unavailable,-1,"ERR","unavailable"},
    {1,8,8,-1,State::Unavailable,-1,"ERR","unavailable"},
    {1,8,8,101,State::Unavailable,-1,"ERR","unavailable"},
    {1,8,8,255,State::Unavailable,-1,"ERR","unavailable"}
  };
  for(const auto &test:cases) {
    const auto reading=battery::decode(test.detection,test.status,test.gauge,test.percent);
    assert(reading.state==test.expected);
    assert(reading.percent==test.expectedPercent);
    char text[32];battery::format(reading,text,sizeof(text));
    assert(std::strcmp(text,test.text)==0);
    assert(std::strcmp(battery::stateName(reading.state),test.name)==0);
  }
  char shortBuffer[1]{'x'};
  battery::format({State::Ready,100},shortBuffer,sizeof(shortBuffer));
  assert(shortBuffer[0]=='\0');
}
