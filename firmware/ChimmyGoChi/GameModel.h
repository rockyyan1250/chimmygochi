#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace game {
constexpr uint32_t kMaxOffline = 12 * 3600;
constexpr int64_t kEarliest = 1704067200;
constexpr int64_t kLatest = 4102444799;
enum class Mode : uint8_t { Game, Ambient, Clock };
enum class Result : uint8_t { Ok, Sleeping, Full, Tired, Busy, Invalid };
struct State {
  int32_t hunger = 7500, mood = 8000, energy = 8000, sleepy = 1500;
  uint32_t bond = 0, remainder = 0, sleepSeconds = 0;
  uint64_t played = 0, generation = 0;
  int64_t savedUtc = 0;
  uint8_t background = 0, brightness = 140, volume = 25;
  Mode startMode = Mode::Ambient;
  bool sleeping = false, autoOff = true, hour24 = true, nightMute = true;
  uint8_t offlineHours = 12;
};
inline int32_t bounded(int64_t n) { return static_cast<int32_t>(std::max<int64_t>(0, std::min<int64_t>(10000, n))); }
inline bool validUtc(int64_t n) { return n >= kEarliest && n <= kLatest; }
inline void advance(State &s, uint32_t seconds) {
  s.played += seconds;
  const uint64_t total = uint64_t(s.remainder) + seconds;
  const uint32_t minutes = total / 60;
  s.remainder = total % 60;
  s.hunger = bounded(int64_t(s.hunger) - int64_t(minutes) * 12);
  if (s.sleeping) {
    s.sleepSeconds = std::min<uint64_t>(UINT32_MAX, uint64_t(s.sleepSeconds) + seconds);
    s.energy = bounded(int64_t(s.energy) + int64_t(minutes) * 90);
    s.sleepy = bounded(int64_t(s.sleepy) - int64_t(minutes) * 100);
  } else {
    s.energy = bounded(int64_t(s.energy) - int64_t(minutes) * 4);
    s.sleepy = bounded(int64_t(s.sleepy) + int64_t(minutes) * 10);
    s.mood = bounded(int64_t(s.mood) - int64_t(minutes) * 5);
  }
}
inline uint32_t offline(State &s, int64_t now) {
  uint32_t applied = 0;
  if (validUtc(now) && validUtc(s.savedUtc) && now >= s.savedUtc) {
    applied = static_cast<uint32_t>(std::min<int64_t>(now - s.savedUtc, uint32_t(s.offlineHours) * 3600));
    advance(s, applied);
  }
  s.savedUtc = validUtc(now) ? now : 0;
  return applied;
}
constexpr std::array<int, 6> kFood = {1800, 1200, 1000, 800, 1500, 500};
inline Result feed(State &s, uint8_t food) {
  if (food >= kFood.size()) return Result::Invalid;
  if (s.sleeping) return Result::Sleeping;
  if (s.hunger >= 9500) return Result::Full;
  s.hunger = bounded(s.hunger + kFood[food]);
  s.mood = bounded(s.mood + 200);
  s.bond = std::min<uint32_t>(1000000, s.bond + 5);
  return Result::Ok;
}
inline Result play(State &s, uint8_t toy) {
  if (toy > 1) return Result::Invalid;
  if (s.sleeping) return Result::Sleeping;
  if (s.energy < 800) return Result::Tired;
  s.energy = bounded(s.energy - (toy ? 500 : 800));
  s.mood = bounded(s.mood + 1200);
  s.bond = std::min<uint32_t>(1000000, s.bond + 8);
  return Result::Ok;
}
inline uint32_t crc32(const uint8_t *p, size_t size, uint32_t crc = 0xffffffff) {
  for (size_t i = 0; i < size; ++i) {
    crc ^= p[i];
    for (unsigned j = 0; j < 8; ++j) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
  }
  return crc;
}
using Record = std::array<uint8_t, 96>;
inline void put(Record &r, size_t at, uint64_t value, size_t n) {
  for (size_t i = 0; i < n; ++i) r[at + i] = uint8_t(value >> (i * 8));
}
inline uint64_t get(const Record &r, size_t at, size_t n) {
  uint64_t v = 0;
  for (size_t i = 0; i < n; ++i) v |= uint64_t(r[at + i]) << (8 * i);
  return v;
}
inline Record encode(const State &s) {
  Record r{};
  memcpy(r.data(), "CGS1", 4);
  put(r, 4, 1, 2); put(r, 6, r.size(), 2);
  put(r, 8, s.generation, 8); put(r, 16, s.savedUtc, 8);
  put(r, 24, s.played, 8);
  put(r, 32, s.hunger, 2); put(r, 34, s.mood, 2);
  put(r, 36, s.energy, 2); put(r, 38, s.sleepy, 2);
  put(r, 40, s.bond, 4); put(r, 44, s.remainder, 4);
  put(r, 48, s.sleepSeconds, 4);
  r[52] = s.background; r[53] = s.brightness; r[54] = s.volume;
  r[55] = uint8_t(s.startMode); r[56] = s.sleeping; r[57] = s.autoOff;
  r[58] = s.hour24; r[59] = s.nightMute; r[60] = s.offlineHours;
  put(r, 92, ~crc32(r.data(), 92), 4);
  return r;
}
inline bool decode(const Record &r, State &out) {
  if (memcmp(r.data(), "CGS1", 4) || get(r, 4, 2) != 1 || get(r, 6, 2) != r.size() ||
      get(r, 92, 4) != uint32_t(~crc32(r.data(), 92))) return false;
  State s;
  s.generation = get(r, 8, 8); s.savedUtc = get(r, 16, 8); s.played = get(r, 24, 8);
  s.hunger = get(r, 32, 2); s.mood = get(r, 34, 2); s.energy = get(r, 36, 2); s.sleepy = get(r, 38, 2);
  s.bond = get(r, 40, 4); s.remainder = get(r, 44, 4); s.sleepSeconds = get(r, 48, 4);
  s.background = r[52]; s.brightness = r[53]; s.volume = r[54]; s.startMode = Mode(r[55]);
  s.sleeping = r[56]; s.autoOff = r[57]; s.hour24 = r[58]; s.nightMute = r[59]; s.offlineHours = r[60];
  if (s.hunger > 10000 || s.mood > 10000 || s.energy > 10000 || s.sleepy > 10000 ||
      s.bond > 1000000 || s.remainder >= 60 || s.background >= 3 || s.volume > 100 ||
      s.brightness < 20 || s.brightness > 220 || r[55] > 2 || r[56] > 1 || r[57] > 1 ||
      r[58] > 1 || r[59] > 1 || s.offlineHours < 1 || s.offlineHours > 24 ||
      (s.savedUtc != 0 && !validUtc(s.savedUtc))) return false;
  out = s;
  return true;
}
inline bool safeName(const char *s) {
  if (!s || !*s || strlen(s) > 48 || strstr(s, "..")) return false;
  for (; *s; ++s) {
    if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') ||
          *s == '_' || *s == '-' || *s == '.')) return false;
  }
  return true;
}
struct ImageHeader { uint16_t width, height, frames; uint8_t bpp; size_t bytes; };
inline bool imageHeader(const uint8_t *p, size_t fileSize, ImageHeader &h) {
  if (fileSize < 16 || memcmp(p, "CGA1", 4)) return false;
  auto u16 = [p](size_t o) { return uint16_t(p[o] | uint16_t(p[o+1]) << 8); };
  h.width=u16(4); h.height=u16(6); h.frames=u16(8); h.bpp=p[10];
  if (h.width == 0 || h.height == 0 || h.width > 480 || h.height > 480 ||
      h.frames == 0 || h.frames > 16 || (h.bpp != 2 && h.bpp != 3)) return false;
  for (size_t i=11; i<16; ++i) if(p[i]) return false;
  h.bytes = size_t(h.width) * h.height * h.frames * h.bpp;
  return h.bytes <= 4 * 1024 * 1024 && fileSize == h.bytes + 16;
}
}  // namespace game
