#pragma once
#include <Arduino.h>

// KMB (九巴) next-bus fetch from data.etabus.gov.hk, run on its own task so the UI keeps
// animating during the TLS handshake. One batch (up to two stops) at a time.
namespace kmb {
constexpr unsigned kMaxArrivals=3,kMaxBoards=2;
struct Request {
  char route[8];
  char bound;  // 'O' / 'I'; 0 accepts either direction
  uint8_t serviceType;
  char stop[20];
};
struct Arrival {
  char dest[64];    // dest_tc
  char remark[64];  // rmk_tc
  int64_t eta;      // UTC epoch seconds; 0 when the feed has no time (e.g. service ended)
};
struct Board {
  bool ok=false;
  int http=0;
  char error[48]{};
  Arrival arrivals[kMaxArrivals]{};
  uint8_t count=0;
};
struct Result {
  bool done=false;
  uint32_t finishedMs=0;  // millis() when the batch ended
  Board boards[kMaxBoards]{};
  uint8_t boardCount=0;
  uint32_t requestMs=0;
  // Internal (non-PSRAM) heap: the scarce resource TLS competes with LVGL for.
  uint32_t heapBefore=0,heapLowAfter=0,stackFree=0;
};
bool start(const Request *requests,unsigned count);  // false if busy, offline or invalid
bool busy();
Result last();
}
