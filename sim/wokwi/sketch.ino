/*
 * ============================================================
 *  STM32 健康监测仪 · Wokwi 仿真版 (STM32duino / Arduino 框架)
 *  对应实物固件: firmware/MAX30102 (Keil MDK 标准库版 main.c)
 * ------------------------------------------------------------
 *  功能: DS18B20 体温 + 心率/血氧 + MPU6050 跌倒检测
 *        + OLED(128x64 SSD1306, 中文显示) + 蜂鸣器报警
 *
 *  与原版逻辑一致的地方:
 *    - 跌倒判断: 相邻两次采样加速度突变 >= 500 LSB (@±2g) -> 0.30 m/s^2
 *    - 报警条件: 跌倒 或 体温 > 32 ℃ (体温报警时“是否跌倒”行仍显示 否)
 *    - DS18B20 取整数摄氏度(原代码为原始值/16 截断)
 *    - OLED 布局与中文文案与实物完全一致
 *
 *  仅仿真适配的差异:
 *    - OLED    : 实物 PA5/PA6 软件I2C   -> 仿真 B6(SCL)/B7(SDA) 硬件I2C
 *    - MPU6050 : 实物 PB10/PB11(I2C2)   -> 仿真与 OLED 共用一条 I2C 总线
 *    - MAX30102: Wokwi 无该器件模型     -> 用模拟数据源代替(见 loop 第3步)
 *    - 蜂鸣器  : 实物 PA12 低电平有效   -> 仿真 B1 用 PWM 驱动无源蜂鸣器模型
 *    - 跌倒报警保持 3 秒(原版只闪一个周期, 便于仿真观察)
 *
 *  使用方法: wokwi.com -> 新建 "STM32 Blue Pill" 项目 ->
 *            把本文件 / diagram.json / libraries.txt / cn16.h
 *            依次粘贴到对应文件 -> 点运行;
 *            运行中点击 MPU6050、DS18B20 拖动滑块即可交互。
 * ============================================================
 */

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "cn16.h"

/* ---------------- 引脚(与 diagram.json 对应) ---------------- */
#define BUZZER_PIN  PB1     // 实物为 PA12(低电平有效), 仿真改用 PWM 引脚
#define ONEWIRE_PIN PA8     // DS18B20 DQ, 与实物一致
// I2C: 蓝板默认 SCL=B6 / SDA=B7 (OLED 与 MPU6050 共用)

#define OLED_ADDR   0x3C
#define SCREEN_W    128
#define SCREEN_H    64

Adafruit_SSD1306 oled(SCREEN_W, SCREEN_H, &Wire, -1);
OneWire oneWire(ONEWIRE_PIN);
DallasTemperature ds18b20(&oneWire);
Adafruit_MPU6050 mpu;

/* ---------------- 状态变量 ---------------- */
int  Temp = 0;              // 体温(整数℃, 同原代码)
int  hr = 0, spo2 = 0;      // 心率/血氧(MAX30102 模拟源)
float lastAx = 0, lastAy = 0;
unsigned long fallUntilMs = 0;   // 跌倒报警保持到该时刻
unsigned long lastTempMs = 0, lastVitalMs = 0, lastDrawMs = 0;
bool alarmPrev = false;

// 跌倒阈值: 原代码 500 LSB @±2g = 500/16384 g ≈ 0.30 m/s^2
const float FALL_DELTA_MS2 = 0.30f;

/* ---------------- 中文绘制(复用江协科技 16x16 字模) ---------------- */
static const CnCell16* findCn(const char* s3) {
  for (int i = 0; i < CN16_COUNT; i++) {
    const char* idx = CN16[i].idx;
    if (idx[0] == s3[0] && idx[1] == s3[1] && idx[2] == s3[2]) return &CN16[i];
  }
  return nullptr;
}

// 字模为“页-列”格式(每列2字节: 上8行/下8行, LSB=顶行),
// 转成 Adafruit_GFX drawBitmap 的行优先格式(MSB=最左)后绘制
static void drawCnChar(int16_t x, int16_t y, const char* s3) {
  const CnCell16* cell = findCn(s3);
  if (!cell) return;
  static uint8_t buf[32];
  memset(buf, 0, sizeof(buf));
  for (uint8_t c = 0; c < 16; c++) {
    for (uint8_t b = 0; b < 8; b++) {
      if ((cell->data[c] >> b) & 0x01)      buf[b * 2 + (c >> 3)]       |= (0x80 >> (c & 7));
      if ((cell->data[16 + c] >> b) & 0x01) buf[(b + 8) * 2 + (c >> 3)] |= (0x80 >> (c & 7));
    }
  }
  oled.drawBitmap(x, y, buf, 16, 16, SSD1306_WHITE);
}

// 绘制 16x16 中文字符串(UTF-8, 每字 3 字节)
static void drawCnText(int16_t x, int16_t y, const char* s) {
  while (*s) {
    drawCnChar(x, y, s);
    s += 3;
    x += 16;
  }
}

// 两位整数(对应原 OLED_ShowNum, 固定 2 位, 前导 0)
static void drawNum(int16_t x, int16_t y, int v) {
  char b[8];
  snprintf(b, sizeof(b), "%02d", v);
  oled.setTextSize(2);
  oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(x, y);
  oled.print(b);
}

// 小号 ASCII 后缀(BPM / % 等)
static void drawSmall(int16_t x, int16_t y, const char* s) {
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(x, y + 4);
  oled.print(s);
}

// 刷新整屏(布局与实物一致)
static void drawUI(bool fallActive) {
  oled.clearDisplay();
  drawCnText(0, 0,  "温度："); drawNum(56, 0,  Temp);   drawCnText(88, 0,  "℃");
  drawCnText(0, 16, "心率："); drawNum(56, 16, hr);     drawSmall(88, 16, "BPM");
  drawCnText(0, 32, "血氧："); drawNum(56, 32, spo2);   drawSmall(88, 32, "%");
  drawCnText(0, 48, "是否跌倒：");
  drawCnText(80, 48, fallActive ? "是" : "否");
  oled.display();
}

/* ---------------- 初始化 ---------------- */
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println("=== STM32 健康监测仪 · Wokwi 仿真 ===");

  pinMode(BUZZER_PIN, OUTPUT);
  analogWrite(BUZZER_PIN, 0);

  Wire.setSDA(PB7);        // 蓝板 I2C: SDA=B7, SCL=B6
  Wire.setSCL(PB6);
  Wire.begin();

  oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  oled.clearDisplay();
  oled.display();

  ds18b20.begin();
  ds18b20.setWaitForConversion(false);   // 异步转换, 不阻塞主循环

  while (!mpu.begin()) {
    Serial.println("MPU6050 未连接, 重试...");
    delay(500);
  }
  mpu.setAccelerometerRange(MPU6050_RANGE_2_G);   // 与原代码 ±2g 量程一致
  mpu.setGyroRange(MPU6050_RANGE_250_DEG);
  mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);
  Serial.println("MPU6050 就绪");

  randomSeed(millis());
}

/* ---------------- 主循环(对应原 main.c while(1)) ---------------- */
void loop() {
  unsigned long now = millis();

  /* 1) DS18B20 体温: 每 1.1s 读一次, 取整数℃(原代码: 原始值/16) */
  if (now - lastTempMs >= 1100) {
    ds18b20.requestTemperatures();
    float tc = ds18b20.getTempCByIndex(0);   // 读到上一次转换结果
    if (tc > -55.0f && tc < 60.0f) Temp = (int)tc;   // 过滤未就绪/非法读数
    lastTempMs = now;
  }

  /* 2) MPU6050 跌倒检测: 相邻采样加速度突变(原 500 LSB @±2g) */
  sensors_event_t a, g, t;
  mpu.getEvent(&a, &g, &t);
  float ax = a.acceleration.x, ay = a.acceleration.y;
  if (fabsf(ax - lastAx) >= FALL_DELTA_MS2 || fabsf(ay - lastAy) >= FALL_DELTA_MS2) {
    fallUntilMs = now + 3000;                // 跌倒报警保持3秒(便于观察)
  }
  lastAx = ax;
  lastAy = ay;
  bool fallActive = (now < fallUntilMs);

  /* 3) 心率/血氧: MAX30102 在 Wokwi 无模型, 用模拟数据源代替(实物为 Maxim 算法真实值) */
  if (now - lastVitalMs >= 1000) {
    if (hr == 0) { hr = 72; spo2 = 98; }     // 开机先显示 00, 之后出数(同原版)
    hr   = constrain(hr   + (int)random(-2, 3), 62, 88);
    spo2 = constrain(spo2 + (int)random(-1, 2), 95, 99);
    lastVitalMs = now;
  }

  /* 4) 报警: 跌倒 或 体温>32℃ -> 蜂鸣器(实物为低电平有效的有源蜂鸣器) */
  bool alarm = fallActive || (Temp > 32);
  if (alarm != alarmPrev) {
    analogWrite(BUZZER_PIN, alarm ? 128 : 0);
    alarmPrev = alarm;
  }

  /* 5) 显示 + 串口(每 250ms 刷新, 对应原版 OLED_Update) */
  if (now - lastDrawMs >= 250) {
    drawUI(fallActive);
    char msg[96];
    snprintf(msg, sizeof(msg), "温度:%d℃  心率:%d  血氧:%d%%  跌倒:%s  报警:%s",
             Temp, hr, spo2, fallActive ? "是" : "否", alarm ? "开" : "关");
    Serial.println(msg);
    lastDrawMs = now;
  }
}