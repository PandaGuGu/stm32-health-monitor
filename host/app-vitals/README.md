# 健康监测上位机（Android）

面向 STM32 健康监测仪的安卓客户端，直连 ESP32 的 HTTP 接口，不经过电脑。

- 包名 `com.sit.vitals`，`minSdk 24` / `targetSdk 35`
- 零第三方依赖：只用 `HttpURLConnection` + 平台自带 `org.json` + 两个自绘 `View`
- 产物 `app-vitals.apk` 约几十 KB

## 数据来源

轮询 `GET http://<esp32-ip>/api/status`（默认 `192.168.4.1`），只取 `vitals` 段：

```json
"vitals":{"ok":true,"bpm":88,"spo2":99,"temp":28.2,"fall":0,
          "src":"uart","age_ms":722,"frames":310,"bad":1,"invalid":0,"uart":true}
```

## 关键显示原则

**只信 `ok == true` 的读数。** STM32 的 PPG 门控失败时，`vitals` 里的 `bpm`/`spo2`
保留的是上一次的有效值，ESP32 也不会刷新 `age_ms` 的计时起点（见
[`docs/esp32/protocol.md`](../../docs/esp32/protocol.md) §8）。因此：

| 状态 | 心率 / 血氧 | 体温 / 跌倒 |
|---|---|---|
| `ok = true` | 显示数值，异常值标红 | 显示数值 |
| `ok = false` | 显示 `—` 与「无读数」，**绝不显示 0 或残留旧值** | 照常显示（两个传感器独立于 PPG 门控） |

阈值：心率 <60 或 >100 标红（偏慢/偏快），血氧 <95 标红（偏低），跌倒为 1 时标红并震动一次。

趋势曲线只连有效采样点，无读数时段直接断开——一眼能看出门控什么时候丢了信号。

## 界面

```
健康监测                    已连接 · uart
[http://192.168.4.1]        [连接]
┌──────────┬──────────┐
│ 心率 · BPM│ 血氧 · %  │
│   88     │   99     │
│   正常    │   正常    │
├──────────┼──────────┤
│ 体温 · ℃  │ 是否跌倒   │
│  28.2    │   否      │
└──────────┴──────────┘
┌──────────────────────┐
│ 心率趋势（90 点滚动）   │
└──────────────────────┘
数据 0.7s 前 · 有效帧 310 · 坏帧 1 · 无读数帧 0 · 运行 629s
```

- 地址栏可改，回车或点「连接」重连，地址存 `SharedPreferences`
- 轮询间隔 1 s（STM32 每 2 s 发一帧，1 s 轮询足够及时）
- 屏幕常亮；`onPause` 停止轮询，`onResume` 自动恢复

## 使用

1. 手机连 ESP32 热点 `esp32-info-XXXX`（密码 `12345678`）
2. 安装 APK 后打开，地址保持默认 `http://192.168.4.1`
3. 需要看数据时用手指压住 MAX30102，1–2 个采集周期后出数

## 构建

```bash
cd host/app-vitals
gradle assembleDebug          # 或直接用 Android Studio 打开本目录
```

命令行手工构建（无 gradle 时）：

```bash
ANDROID_HOME=~/tools/android-sdk
BT=$ANDROID_HOME/build-tools/34.0.0
PLATFORM=$ANDROID_HOME/platforms/android-34/android.jar

$BT/aapt2 compile --dir app/src/main/res -o /tmp/res.zip
$BT/aapt2 link -o /tmp/base.apk -I $PLATFORM --manifest app/src/main/AndroidManifest.xml \
    -R /tmp/res.zip --java /tmp/gen --min-sdk-version 24 --target-sdk-version 34
javac -source 17 -target 17 -cp $PLATFORM -d /tmp/classes \
    $(find app/src/main/java /tmp/gen -name '*.java')
$BT/d8 --lib $PLATFORM --min-api 24 --output /tmp/dex $(find /tmp/classes -name '*.class')
cd /tmp/dex && zip -j /tmp/base.apk classes.dex
$BT/apksigner sign --ks ~/.android/debug.keystore --ks-pass pass:android /tmp/base.apk
```

## 已知限制

- 只做只读展示，不改 ESP32 配置（GPIO/扫描/命令台仍在 `host/web` 与 `host/app`）
- 不支持 HTTPS（ESP32 侧是明文 HTTP，Manifest 已开 `usesCleartextTraffic`）
- 未实现断线自动切换地址，需手点「连接」
