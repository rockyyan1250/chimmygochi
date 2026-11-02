#ifndef LV_CONF_H
#define LV_CONF_H
#include <stdint.h>
#define LV_COLOR_DEPTH 16
#define LV_COLOR_16_SWAP 0
// LVGL objects live in PSRAM: a fixed 128KB pool in internal RAM left too little for Wi-Fi + TLS.
#define LV_MEM_CUSTOM 1
#define LV_MEM_CUSTOM_INCLUDE <esp_heap_caps.h>
#define LV_MEM_CUSTOM_ALLOC(size) heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define LV_MEM_CUSTOM_FREE heap_caps_free
#define LV_MEM_CUSTOM_REALLOC(p, size) heap_caps_realloc(p, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define LV_TICK_CUSTOM 1
#define LV_TICK_CUSTOM_INCLUDE "esp_timer.h"
#define LV_TICK_CUSTOM_SYS_TIME_EXPR ((uint32_t)(esp_timer_get_time() / 1000))
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_MONTSERRAT_48 1
#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
#define LV_USE_QRCODE 1
#define LV_USE_DEMO_WIDGETS 0
#define LV_USE_DEMO_MUSIC 0
#endif
