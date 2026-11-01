#pragma once
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <Wire.h>
#include <TouchDrvCSTXXX.hpp>
#define XPOWERS_CHIP_AXP2101
#include <XPowersLib.h>
#include <lvgl.h>
#include <atomic>
#include "Battery.h"
#include "GameModel.h"

extern Arduino_CO5300 display;
extern XPowersPMU pmu;
extern TouchDrvCST92xx touch;
extern bool pmuOk, touchOk, audioOk, displayOn;
extern bool chargeConfigured,chargeHot;  // charge limits verified; paused for heat
void chargeGuard();
extern uint32_t lastInput;
extern uint32_t touchCount, flushCount, maxFlushUs;
extern std::atomic<uint32_t> audioFailures;
extern uint16_t *screenPixels;
bool boardBegin();
battery::Reading readBattery();
void setDisplay(bool on, uint8_t brightness);
bool rtcRead(int64_t &epoch);
bool rtcWrite(int64_t epoch);
bool playSound(const uint8_t *wav, size_t length, uint8_t volume);
void stopSound();
void readTouch(lv_indev_drv_t *, lv_indev_data_t *);
void screenFlush(lv_disp_drv_t *, const lv_area_t *, lv_color_t *);
