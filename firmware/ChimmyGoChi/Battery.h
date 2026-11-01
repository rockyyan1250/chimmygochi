#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

namespace battery {
enum class State : uint8_t { Ready, Disconnected, Unavailable };

struct Reading {
  State state;
  int percent;
};

inline Reading decode(int detection,int status,int gauge,int percent) {
  if(detection<0||detection>255||!(detection&0x01)||status<0||status>255)
    return {State::Unavailable,-1};
  if(!(status&0x08)) return {State::Disconnected,-1};
  if(gauge<0||gauge>255||!(gauge&0x08)||percent<0||percent>100)
    return {State::Unavailable,-1};
  return {State::Ready,percent};
}

inline const char *stateName(State state) {
  switch(state) {
    case State::Ready:return "ready";
    case State::Disconnected:return "disconnected";
    case State::Unavailable:return "unavailable";
  }
  return "unavailable";
}

inline void format(const Reading &reading,char *buffer,size_t size) {
  if(reading.state==State::Ready) snprintf(buffer,size,"%d%%",reading.percent);
  else if(reading.state==State::Disconnected) snprintf(buffer,size,"--");
  else snprintf(buffer,size,"ERR");
}
}
