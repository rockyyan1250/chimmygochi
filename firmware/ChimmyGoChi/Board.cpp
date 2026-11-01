#include "Board.h"
#include <ESP_I2S.h>
#include <esp_timer.h>
#include <time.h>
#include "es8311.h"

Arduino_ESP32QSPI displayBus(12,38,4,5,6,7);
Arduino_CO5300 display(&displayBus,39,0,480,480,0,0,0,0);
XPowersPMU pmu;
TouchDrvCST92xx touch;
bool pmuOk=false,touchOk=false,audioOk=false,displayOn=true;
bool chargeConfigured=false,chargeHot=false;
uint32_t lastInput=0,touchCount=0,flushCount=0,maxFlushUs=0;
std::atomic<uint32_t> audioFailures{0};
uint16_t *screenPixels=nullptr;
static I2SClass audio;
static QueueHandle_t soundQueue=nullptr;
static std::atomic<uint32_t> soundGeneration{0};
struct Sound { uint8_t *data; size_t length; uint8_t volume; uint32_t generation; };
static void audioTask(void *) {
  Sound s;
  for (;;) {
    if(xQueueReceive(soundQueue,&s,portMAX_DELAY)!=pdTRUE) continue;
    int16_t stereo[512];
    bool failed=false;
    for(size_t offset=44;offset<s.length && s.generation==soundGeneration.load();offset+=512) {
      size_t count=std::min<size_t>(256,(s.length-offset)/2);
      for(size_t i=0;i<count;i++) {
        int16_t sample=int16_t(uint16_t(s.data[offset+i*2]) | uint16_t(s.data[offset+i*2+1])<<8);
        stereo[2*i]=stereo[2*i+1]=int32_t(sample)*s.volume/100;
      }
      if(audio.write(reinterpret_cast<uint8_t *>(stereo),count*4)!=count*4) {failed=true;break;}
    }
    memset(stereo,0,sizeof(stereo));
    if(audio.write(reinterpret_cast<uint8_t *>(stereo),sizeof(stereo))!=sizeof(stereo)) failed=true;
    if(failed) {audioFailures++; Serial.println("ERROR audio write");}
    free(s.data);
  }
}
void stopSound() { soundGeneration++; }
bool playSound(const uint8_t *wav,size_t len,uint8_t volume) {
  if(!audioOk) return false;
  if(!volume) return true;
  if(!wav||len<44||len>160044||memcmp(wav,"RIFF",4)||memcmp(wav+8,"WAVEfmt ",8)||
     wav[20]!=1||wav[22]!=1||wav[24]!=0x80||wav[25]!=0x3e||wav[34]!=16||memcmp(wav+36,"data",4)) return false;
  auto copy=static_cast<uint8_t *>(heap_caps_malloc(len,MALLOC_CAP_SPIRAM));
  if(!copy) return false;
  memcpy(copy,wav,len);
  Sound s{copy,len,volume,++soundGeneration};
  if(xQueueSend(soundQueue,&s,0)!=pdTRUE) { free(copy); return false; }
  return true;
}
bool boardBegin() {
  Serial.println("INIT I2C/PMIC");
  Wire.begin(15,14,400000);
  Wire.setTimeOut(30);
  pmuOk=pmu.begin(Wire,AXP2101_SLAVE_ADDRESS,15,14);
  if(pmuOk) {
    // Main cell: 1S LiPo 3.7V 1000mAh (103030, MX1.25). Conservative 4.1V / 300mA (0.3C) because
    // the board has no battery thermistor; the coin-cell (backup) charger stays off.
    pmu.disableCellbatteryCharge();
    pmu.disableButtonBatteryCharge();
    pmu.enableGauge();
    pmu.disableTSPinMeasure();pmu.enableTemperatureMeasure();
    pmu.setChargeTargetVoltage(XPOWERS_AXP2101_CHG_VOL_4V1);
    pmu.setChargerConstantCurr(XPOWERS_AXP2101_CHG_CUR_300MA);
    pmu.setPrechargeCurr(XPOWERS_AXP2101_PRECHARGE_50MA);
    pmu.setChargerTerminationCurr(XPOWERS_AXP2101_CHG_ITERM_25MA);
    // Enable only when the limits read back exactly; otherwise charging stays off.
    if(pmu.getChargeTargetVoltage()==XPOWERS_AXP2101_CHG_VOL_4V1&&
       pmu.getChargerConstantCurr()==XPOWERS_AXP2101_CHG_CUR_300MA) pmu.enableCellbatteryCharge();
    Wire.beginTransmission(AXP2101_SLAVE_ADDRESS);Wire.write(0x18);
    if(Wire.endTransmission(false)==0&&Wire.requestFrom(uint8_t(AXP2101_SLAVE_ADDRESS),size_t(1))==1)
      chargeConfigured=(Wire.read()&0x06)==0x02;  // bit1 cell charge on, bit2 coin cell off
    if(!chargeConfigured) {pmu.disableCellbatteryCharge();Serial.println("ERROR charge limits not verified; charging left off");}
    if(!pmu.enableBattDetection()) Serial.println("ERROR battery detection unavailable");
    pmu.enableBattVoltageMeasure(); pmu.enableVbusVoltageMeasure();
    pmu.setPowerKeyPressOffTime(XPOWERS_POWEROFF_6S);
    pmu.setLongPressPowerOFF(); pmu.enableLongPressShutdown();
    pmu.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
    pmu.clearIrqStatus();
    pmu.enableIRQ(XPOWERS_AXP2101_PKEY_SHORT_IRQ|XPOWERS_AXP2101_PKEY_LONG_IRQ);
  } else Serial.println("ERROR PMIC unavailable");
  Serial.println("INIT display");
  if(!display.begin()) {Serial.println("ERROR display init");return false;}
  screenPixels=static_cast<uint16_t *>(heap_caps_calloc(480*480,2,MALLOC_CAP_SPIRAM));
  if(!screenPixels) Serial.println("ERROR diagnostic framebuffer allocation");
  displayBus.writeC8D8(0x36,0xA0);
  display.setBrightness(100); display.fillScreen(RGB565_BLACK);
  Serial.println("INIT touch");
  touch.setPins(40,11);
  touchOk=touch.begin(Wire,CST92XX_SLAVE_ADDRESS,15,14);
  if(touchOk) {
    touch.setMaxCoordinates(480,480);
    touch.setSwapXY(true); touch.setMirrorXY(true,false);
  } else Serial.println("ERROR touch unavailable");
  Wire.setClock(400000);
  Serial.println("INIT audio");
  audio.setPins(9,45,8,-1,42);
  if(audio.begin(I2S_MODE_STD,16000,I2S_DATA_BIT_WIDTH_16BIT,I2S_SLOT_MODE_STEREO)) {
    auto codec=es8311_create(0,ES8311_ADDRRES_0);
    es8311_clock_config_t cfg{false,false,true,4096000,16000};
    if(codec && es8311_init(codec,&cfg,ES8311_RESOLUTION_16,ES8311_RESOLUTION_16)==ESP_OK &&
       es8311_sample_frequency_config(codec,4096000,16000)==ESP_OK &&
       es8311_microphone_config(codec,false)==ESP_OK &&
       es8311_voice_volume_set(codec,75,nullptr)==ESP_OK) {
      soundQueue=xQueueCreate(2,sizeof(Sound));
      audioOk=soundQueue && xTaskCreate(audioTask,"audio",4096,nullptr,2,nullptr)==pdPASS;
    }
  }
  pinMode(46,OUTPUT);digitalWrite(46,audioOk?HIGH:LOW);
  if(!audioOk) Serial.println("ERROR audio init");
  pinMode(18,INPUT_PULLUP);
  return true;
}
battery::Reading readBattery() {
  if(!pmuOk) return {battery::State::Unavailable,-1};
  const int detection=pmu.readRegister(XPOWERS_AXP2101_BAT_DET_CTRL);
  const int status=pmu.readRegister(XPOWERS_AXP2101_STATUS1);
  const int gauge=pmu.readRegister(XPOWERS_AXP2101_CHARGE_GAUGE_WDT_CTRL);
  const int percent=pmu.readRegister(XPOWERS_AXP2101_BAT_PERCENT_DATA);
  return battery::decode(detection,status,gauge,percent);
}
// Without a battery thermistor the PMIC die temperature is the only heat signal:
// pause charging above 60C and resume below 50C.
void chargeGuard() {
  if(!pmuOk||!chargeConfigured) return;
  const float t=pmu.getTemperature();
  if(!chargeHot&&t>60) {pmu.disableCellbatteryCharge();chargeHot=true;Serial.printf("ERROR PMIC %.1fC: charging paused\n",t);}
  else if(chargeHot&&t<50) {pmu.enableCellbatteryCharge();chargeHot=false;Serial.println("NOTICE charging resumed");}
}
void setDisplay(bool on,uint8_t brightness) {
  displayOn=on;
  if(on) {display.displayOn();display.setBrightness(brightness);}
  else {stopSound();display.setBrightness(0);display.displayOff();}
}
void readTouch(lv_indev_drv_t *,lv_indev_data_t *data) {
  static int16_t lastX=0,lastY=0;
  static bool wasDown=false,suppress=false;
  data->point.x=lastX;data->point.y=lastY;data->state=LV_INDEV_STATE_REL;
  if(!touchOk) return;
  int16_t x[5]{},y[5]{};
  const bool down=touch.getPoint(x,y,5)>0;
  if(down) {
    lastX=std::max<int16_t>(0,std::min<int16_t>(479,x[0]));
    lastY=std::max<int16_t>(0,std::min<int16_t>(479,y[0]));
    lastInput=millis();
    if(!wasDown) touchCount++;
    if(!displayOn) {setDisplay(true,140);suppress=true;}
    if(!suppress) {data->state=LV_INDEV_STATE_PR;data->point.x=lastX;data->point.y=lastY;}
  } else suppress=false;
  wasDown=down;
}
void screenFlush(lv_disp_drv_t *drv,const lv_area_t *area,lv_color_t *colors) {
  const int64_t start=esp_timer_get_time();
  if(screenPixels) {
    const unsigned w=area->x2-area->x1+1;
    for(int y=area->y1;y<=area->y2;y++)
      memcpy(screenPixels+y*480+area->x1,colors+(y-area->y1)*w,w*2);
  }
  if(displayOn) display.draw16bitRGBBitmap(area->x1,area->y1,
    reinterpret_cast<uint16_t *>(colors),area->x2-area->x1+1,area->y2-area->y1+1);
  maxFlushUs=std::max<uint32_t>(maxFlushUs,esp_timer_get_time()-start);
  flushCount++;
  lv_disp_flush_ready(drv);
}
static bool readRegs(uint8_t reg,uint8_t *dst,size_t size) {
  Wire.beginTransmission(0x51);Wire.write(reg);
  if(Wire.endTransmission(false)!=0||Wire.requestFrom(uint8_t(0x51),size)!=size) return false;
  for(size_t i=0;i<size;i++) dst[i]=Wire.read();
  return true;
}
bool rtcRead(int64_t &epoch) {
  uint8_t control=0,b[7];
  if(!readRegs(0,&control,1)||(control&0x22)||!readRegs(4,b,7)||(b[0]&0x80)) return false;
  auto dec=[](uint8_t v)->int {return (v&15)>9||(v>>4)>9?-1:(v>>4)*10+(v&15);};
  tm t{};
  t.tm_sec=dec(b[0]&0x7f);t.tm_min=dec(b[1]&0x7f);t.tm_hour=dec(b[2]&0x3f);
  t.tm_mday=dec(b[3]&0x3f);t.tm_mon=dec(b[5]&0x1f)-1;t.tm_year=100+dec(b[6]);
  if(t.tm_sec<0||t.tm_sec>59||t.tm_min<0||t.tm_min>59||t.tm_hour<0||t.tm_hour>23||
     t.tm_mday<1||t.tm_mday>31||t.tm_mon<0||t.tm_mon>11||t.tm_year<124||t.tm_year>199) return false;
  tm original=t;
  // The application fixes TZ to HKT-8; the RTC registers themselves store UTC.
  epoch=int64_t(mktime(&t))+8*3600;
  return game::validUtc(epoch)&&t.tm_mday==original.tm_mday&&t.tm_mon==original.tm_mon;
}
bool rtcWrite(int64_t epoch) {
  if(!game::validUtc(epoch)) return false;
  time_t now=epoch;tm t{};gmtime_r(&now,&t);
  auto bcd=[](int v)->uint8_t {return (v/10)<<4|v%10;};
  uint8_t control;
  if(!readRegs(0,&control,1)) return false;
  Wire.beginTransmission(0x51);Wire.write(0);Wire.write(control&~0x22);
  if(Wire.endTransmission()!=0) return false;
  Wire.beginTransmission(0x51);Wire.write(4);
  uint8_t data[]={bcd(t.tm_sec),bcd(t.tm_min),bcd(t.tm_hour),bcd(t.tm_mday),
    uint8_t(t.tm_wday),bcd(t.tm_mon+1),bcd(t.tm_year-100)};
  Wire.write(data,sizeof(data));
  if(Wire.endTransmission()!=0) return false;
  int64_t check=0;
  return rtcRead(check)&&check>=epoch&&check<=epoch+1;
}
