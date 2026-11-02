#pragma once
#include <Arduino.h>
class WebServer;

// Wi-Fi: up to five remembered networks (newest first, in NVS) with automatic reconnect, plus a
// setup portal: a soft AP whose random password is only shown inside the on-screen QR code, a
// captive DNS and a Cantonese web page where the phone picks the home network.
namespace net {
enum class State : uint8_t { Off, Connecting, Connected, Failed };
enum class Join : uint8_t { Idle, Trying, Joined, Rejected };  // the portal's pending network
constexpr unsigned kMaxSaved=5;

void begin();  // load saved networks and start connecting
void tick();   // from loop(): reconnect state machine, portal DNS/HTTP
State state();
String ssid();
int rssi();
unsigned savedCount();
bool startPortal();
void stopPortal();
bool portalActive();
Join joinState();
const char *portalName();
const char *portalQr();  // "WIFI:T:WPA;S:<name>;P:<password>;;"
bool addNetwork(const char *ssid,const char *password);  // try now; saved once it connects
bool connectedOnce();  // true once after each new connection (for NTP sync)
WebServer &web();  // shared HTTP server (port 80): other modules add their pages
String ip();       // "" unless connected to the home network
}
