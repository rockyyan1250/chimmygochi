#pragma once
#include <lvgl.h>
#include "Assets.h"

// Four short CHIMMY mini games played on their own screen. The game draws on the screen the
// host built (behind the battery pill on the top layer) and reports back through Host.
namespace games {
enum Id : uint8_t { Catch, Lift, Hide, Mimic, kGames };
constexpr const char *names[kGames]={"接零食","頂波波","估下喺邊","跟住跳"};
struct Host {
  lv_obj_t *screen=nullptr;
  const AssetPack *pack=nullptr;
  const lv_font_t *font=nullptr;
  void (*sound)(int slot)=nullptr;       // 0 tap, 1 happy, 2 eat, 3 dress
  void (*reward)(unsigned score)=nullptr;  // once per finished game
  void (*restart)()=nullptr;
  void (*exit)()=nullptr;
};
void start(Id id,const Host &host);
void tick();  // from loop(); paces itself to ~30 fps
void stop();  // the screen is being deleted
bool running();
unsigned best(Id id);
}
