// ============================================================
// ble_link.cpp — 蓝牙链路（可选模块）
//
// 只在 ENABLE_BLE=1 时编译真实实现，否则全是空函数，
// 这样 esp32_info_node.ino 可以无条件调用 Ble::begin()/loop()。
//
// 与局域网 HTTP 的分工：那条是手机/PC 来拉，这条是手机就近来收（不需要同网，也不需要配网）。
// 两条都开时射频分时，吞吐各降一些，但功能互不影响。
// ============================================================
#include "ble_link.h"

#include "config.h"

#if ENABLE_BLE

#include <BLE2902.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>

#include "device_info.h"
#include "json_out.h"
#include "uart_link.h"

#define NUS_SVC "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_RX  "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_TX  "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

namespace {

BLEServer         *s_srv = nullptr;
BLECharacteristic *s_tx  = nullptr;

bool            s_on = false;
volatile bool   s_conn = false;
volatile uint16_t s_connId = 0;
uint16_t        s_mtu = 0;
uint32_t        s_sent = 0;
uint32_t        s_lastPush = 0;
int             s_lastFrames = -1;
String          s_name;

class ServerCb : public BLEServerCallbacks {
  // 只重写带 param 的版本：基类里它默认转发到无参版本，
  // 两个都重写会导致同一次连接被处理两遍
  void onConnect(BLEServer *p, esp_ble_gatts_cb_param_t *param) override {
    s_conn   = true;
    s_connId = param ? param->connect.conn_id : 0;
    Json::log('I', String("ble: connected, conn_id=") + String(s_connId));
  }

  void onDisconnect(BLEServer *p) override {
    s_conn = false;
    s_mtu  = 0;
    Json::log('I', "ble: disconnected, re-advertising");
    p->startAdvertising();          // 断开后必须手动重启广播
  }
};

}  // namespace

void Ble::begin() {
  if (s_on) return;

  String mac = Device::macStr();
  mac.replace(":", "");
  s_name = String(BLE_NAME) + "-" + mac.substring(mac.length() - 4);

  BLEDevice::init(s_name.c_str());
  BLEDevice::setMTU(BLE_MTU);

  s_srv = BLEDevice::createServer();
  s_srv->setCallbacks(new ServerCb());

  BLEService *svc = s_srv->createService(NUS_SVC);

  s_tx = svc->createCharacteristic(NUS_TX, BLECharacteristic::PROPERTY_NOTIFY);
  s_tx->addDescriptor(new BLE2902());   // CCCD，手机靠它打开通知

  // RX 预留给"手机发命令"的场景，当前固件不解析，创建只是为了 UUID 完整
  svc->createCharacteristic(NUS_RX, BLECharacteristic::PROPERTY_WRITE |
                                        BLECharacteristic::PROPERTY_WRITE_NR);
  svc->start();

  BLEAdvertising *adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(NUS_SVC);
  adv->setScanResponse(true);
  adv->setMinPreferred(0x06);
  adv->setMinPreferred(0x12);
  BLEDevice::startAdvertising();

  s_on = true;
  Json::log('I', "ble: advertising as " + s_name);
}

void Ble::loop() {
  if (!s_on) return;

  if (s_conn && s_srv) {
    uint16_t m = s_srv->getPeerMTU(s_connId);
    if (m != s_mtu && m > 0) {
      s_mtu = m;
      Json::log('I', String("ble: mtu=") + String(m));
    }
  }
  if (!s_conn) return;

  // 帧数没变也按 BLE_NOTIFY_MS 兜底推一次，否则 App 无法区分
  // "设备还在但没读数" 和 "设备已经掉了"
  uint32_t now = millis();
  int f = Vitals::frames();
  if (f == s_lastFrames && now - s_lastPush < BLE_NOTIFY_MS) return;
  s_lastFrames = f;
  s_lastPush = now;

  int room = (int)s_mtu - 3;            // ATT 头占 3 字节
  if (room < 40) return;                // MTU 还没协商出来，等下一次

  String body = "{" + Vitals::fields() + "}";
  if ((int)body.length() > room) {
    if (s_sent % 30 == 0) {
      Json::log('W', String("ble: payload ") + String(body.length()) +
                          " > room " + String(room));
    }
    return;
  }

  s_tx->setValue((uint8_t *)body.c_str(), body.length());
  s_tx->notify();
  s_sent++;
}

void     Ble::setEnabled(bool on) { s_on = on; }
bool     Ble::enabled()           { return s_on; }
bool     Ble::connected()         { return s_conn; }
int      Ble::mtu()               { return s_mtu; }
uint32_t Ble::notifies()          { return s_sent; }
String   Ble::name()              { return s_name; }

#else  // !ENABLE_BLE —— 空实现，保证 sketch 无条件调用也能链接

void     Ble::begin()             {}
void     Ble::loop()              {}
void     Ble::setEnabled(bool)    {}
bool     Ble::enabled()           { return false; }
bool     Ble::connected()         { return false; }
int      Ble::mtu()               { return 0; }
uint32_t Ble::notifies()          { return 0; }
String   Ble::name()              { return String(BLE_NAME); }

#endif  // ENABLE_BLE
