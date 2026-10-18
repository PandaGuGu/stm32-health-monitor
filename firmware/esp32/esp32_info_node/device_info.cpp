// ============================================================
// device_info.cpp — esp_chip_info / efuse MAC / SDK / 复位原因
// ============================================================
#include "device_info.h"

#include <esp_chip_info.h>
#include <esp_mac.h>
#include <esp_system.h>

#include "config.h"
#include "json_out.h"

namespace {
String g_fields;
String g_mac;
String g_reset;

const char *resetReasonName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:   return "poweron";
    case ESP_RST_EXT:       return "ext_pin";
    case ESP_RST_SW:        return "sw";
    case ESP_RST_PANIC:     return "panic";
    case ESP_RST_INT_WDT:   return "int_wdt";
    case ESP_RST_TASK_WDT:  return "task_wdt";
    case ESP_RST_WDT:       return "wdt";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_SDIO:      return "sdio";
    default:                return "unknown";
  }
}

String macToStr(const uint8_t *m) {
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
           m[0], m[1], m[2], m[3], m[4], m[5]);
  return String(buf);
}

}  // namespace

void Device::begin() {
  esp_chip_info_t info;
  esp_chip_info(&info);

  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  g_mac = macToStr(mac);
  g_reset = resetReasonName(esp_reset_reason());

  // 能力位
  String feats;
  if (info.features & CHIP_FEATURE_WIFI_BGN) feats += feats.length() ? ",\"wifi\"" : "\"wifi\"";
  if (info.features & CHIP_FEATURE_BT)       feats += feats.length() ? ",\"bt\""   : "\"bt\"";
  if (info.features & CHIP_FEATURE_BLE)      feats += feats.length() ? ",\"ble\""  : "\"ble\"";

  uint32_t flashKb = ESP.getFlashChipSize() / 1024;
  uint32_t psramKb = ESP.getPsramSize() / 1024;
  uint32_t freqMhz = getCpuFrequencyMhz();

  String f;
  f += "\"fw\":" + Json::q(FW_VERSION);
  f += ",\"build\":" + Json::q(__DATE__ " " __TIME__);
  f += ",\"model\":" + Json::q(String(ESP.getChipModel()));
  f += ",\"rev\":" + String(ESP.getChipRevision());
  f += ",\"cores\":" + String(ESP.getChipCores());
  f += ",\"freq\":" + String(freqMhz);
  f += ",\"flash_kb\":" + String(flashKb);
  f += ",\"psram_kb\":" + String(psramKb);
  f += ",\"sdk\":" + Json::q(String(ESP.getSdkVersion()));
  f += ",\"mac\":" + Json::q(g_mac);
  f += ",\"chip_id\":" + Json::q(g_mac.substring(0, 2) + g_mac.substring(3, 5) +
                                 g_mac.substring(6, 8) + g_mac.substring(9, 11) +
                                 g_mac.substring(12, 14) + g_mac.substring(15, 17));
  f += ",\"reset\":" + Json::q(g_reset);
  f += ",\"features\":[" + feats + "]";
  g_fields = f;
}

String        Device::fields()    { return g_fields; }
String        Device::macStr()    { return g_mac; }
const String &Device::resetName() { return g_reset; }

void Device::emitHello() {
  Json::emit(Json::head("hello") + "," + g_fields + "}");
}
