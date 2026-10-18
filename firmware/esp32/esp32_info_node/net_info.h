// ============================================================
// net_info.h — WiFi 状态 / 扫描 / 凭据持久化（NVS）
// ============================================================
#pragma once
#include <Arduino.h>

namespace Net {

void   begin();
void   loop();

String fields();                 // "mode":"sta",...
void   emitNet();

int    scanAndEmit(uint32_t *durMs);   // 返回 AP 数；-1 表示射频关闭
void   emitScan();

bool   connectSta(const String &ssid, const String &pass, String &errCode);
bool   startAp(const String &ssid, const String &pass, String &errCode);
void   radioOff();
bool   saveCredentials(const String &ssid, const String &pass);

bool   connected();
int    rssi();
String ip();
String modeStr();

}  // namespace Net
