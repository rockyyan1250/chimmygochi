#pragma once
#include <Arduino.h>
class WebServer;

// Two KMB (九巴) lines for the bus screen, chosen on the phone and kept in NVS. The setup page is
// served over the home Wi-Fi; saving needs the 4-digit PIN shown (and put in the QR) on screen.
namespace bus {
constexpr unsigned kLines=2;
struct Line {
  char route[8];      // "" = slot unused
  char bound;         // 'O' outbound / 'I' inbound
  uint8_t serviceType;
  char stop[20];      // KMB stop id
  char stopName[64];  // name_tc, for display
  char dest[64];      // dest_tc of the chosen direction
};
void begin();                // load lines, make a PIN
void attach(WebServer &web);  // /bus page and /api/bus
const Line &line(unsigned i);
bool used(unsigned i);
const char *pin();
uint32_t revision();  // bumps on every save, so the screen can refresh at once
}
