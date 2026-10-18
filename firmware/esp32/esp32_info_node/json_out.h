// ============================================================
// json_out.h — 极简 JSON 行输出（零第三方依赖）
//   emit()      : 打一行 JSON 到串口；处于捕获态时同时收集，供 HTTP 复用
//   q()/esc()   : 字符串转义
//   ack()/err() : 统一响应构造
// ============================================================
#pragma once
#include <Arduino.h>
#include <vector>

namespace Json {

void   begin(Stream *s);
void   emit(const String &line);

// 捕获：HTTP /api/cmd 需要拿到命令产生的行，不能只打串口
void   startCapture();
std::vector<String> stopCapture();

String esc(const String &in);        // 转义但不加引号
String q(const String &in);          // 转义 + 加引号
String q(const char *in);
String num(double v, int dec = 2);

void   log(char lvl, const String &msg);
void   ack(const String &id, const String &cmd, bool ok, const String &dataJson = "");
void   err(const String &id, const String &cmd, const String &code, const String &msg);

// 通用信封 {v,t,ts}
String head(const String &type);

}  // namespace Json
