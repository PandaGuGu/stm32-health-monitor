# 更新日志

格式参考 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，版本号遵循
[语义化版本](https://semver.org/lang/zh-CN/)。

## [v0.0.1] - 2026-10-17

首个可交付版本：**实物四路监测跑通 + Proteus 仿真可完整演示**。

### 新增

- **实物固件** `firmware/MAX30102/`（正点原子标准库 Keil 工程 `IIC.uvprojx`）
  - MAX30102 心率 / 血氧（软件 I2C，Maxim 官方算法，500 点缓冲）
  - DS18B20 体温（单总线，PA8，带外部 4.7 kΩ 上拉）
  - MPU6050 跌倒判定（合矢量 |a| 去重力偏差，阈值 3072 LSB ≈ 2.5 g，连续 2 拍确认）
  - 0.96" OLED 显示（SSD1306，软 I2C，中文字库）+ 有源蜂鸣器报警（PA12 低电平）
- **GCC 构建系统** `firmware/MAX30102/gcc/`：IIC / APP / LIVE / DIAG 四种变体，可脱离 Keil 授权编译
- **仿真工程** `sim/proteus/SIT_sim.pdsprj` + **仿真专用固件** `firmware/MAX30102_sim/`
  - 温度走高精度电位器 RV2 → PA0(ADC1_IN0)，0~3.3 V 映射 15.0~45.0 ℃，一位小数显示
  - 心率内置节律 66~90 BPM；跌倒由 SW3(PB1) 触发；蜂鸣器跟随报警
  - 上电自动扫描 48 组单总线时序，能读到真传感器就自动切换数据源
  - 屏幕标记数据来源：`POT` / `SIM` / `DS18`
- **Wokwi 网页方案** `sim/wokwi/`（diagram.json + sketch.ino）
- **文档**：架构说明、系统原理图（SVG + 引脚表）、`docs/debug/` 10 篇调试报告与故障速查表

### 修复

- `ds18b20.c:68` 无超时死等导致 OLED 全黑、程序卡在初始化 → 改用带超时的实现
- 诊断固件的 `pin_survey_port()` 把 PA5/PA6 重配为输入，导致 OLED 永久空写 → 每帧恢复开漏输出
- 跌倒判定频率 0.52 Hz（漏报）→ 87.7 Hz；判据改为倾斜免疫的 |a| 偏差
- 手指移开后仍显示旧心率/血氧 → 算法层 30–220 bpm 门控 + 5 项信号质量门控
- 数字位数变化残留、图标叠字 → 绘制前 `OLED_ClearArea`

### 已知限制

- 仿真工程无 MAX30102 / MPU6050 模型；DS18B20 实际加载 `DS1822.MDF`，单总线命令写不进
  （48 组时序全部失败），因此温度改用 RV2 电位器模拟
- Proteus 的 MCU 模型 `CLOCK_SCALE = 8`（≈64 MHz），与固件假设的 72 MHz 相差 12.5%，
  任何"靠时间算"的量都会偏；`Clock Scale` 改 `9 Times` 可消除
- 本项目为课程/学习性质，**不是医疗器械**，心率/血氧与跌倒阈值未经临床验证

[v0.0.1]: https://github.com/PandaGuGu/stm32-health-monitor/releases/tag/v0.0.1
