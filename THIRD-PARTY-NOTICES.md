# 第三方组件与版权声明

本仓库整体以 [MIT 许可证](LICENSE) 发布，**但不覆盖下列第三方组件**。这些组件保留其原始版权与使用限制。

## 1. 正点原子（ALIENTEK）标准外设库与驱动

以下目录/文件源自 ALIENTEK 开源工程，其文件头版权声明为"仅供学习使用，未授权禁止用于商业用途"：

```
firmware/MAX30102/CORE/            core_cm3.c, startup_stm32f10x_*.s
firmware/MAX30102/STM32F10x_FWLib/ ST 标准外设库 3.5（含 ALIENTEK 整理）
firmware/MAX30102/SYSTEM/          delay / sys / timer / usart
firmware/MAX30102/HARDWARE/        OLED / IIC / MPU6050 / DS18B20 / BUZZER / LED
firmware/MAX30102_sim/             同上（仿真工程为同一套驱动的副本）
```

- 版权：`Copyright (C) 正点原子 2009-2019, All rights reserved.`
- 使用限制：**仅供学习研究，未经授权不得用于商业用途**。

## 2. 芯片厂商代码

| 组件 | 来源 | 许可 |
|---|---|---|
| STM32F10x 标准外设库 | STMicroelectronics | BSD-3-Clause（见源文件头） |
| CMSIS Core (`core_cm3.h/.c`) | ARM Limited | Apache-2.0 |
| MAX30102 心率/血氧算法（`HARDWARE/IIC/algorithm.c`） | Maxim Integrated 参考实现 | Maxim 原始声明 |
| SSD1306 OLED 驱动与字库 | 江协科技（jiangxiekeji.com） | 原作者声明 |

## 3. 本项目自有部分

以下内容由本项目成员编写，适用仓库根目录的 MIT 许可证：

- `firmware/MAX30102_sim/USER/main.c`（仿真专用固件逻辑）
- `firmware/MAX30102/USER/main.c` 中的业务逻辑与跌倒判定（基于原始工程二次修改）
- `firmware/MAX30102/gcc/`（GCC 构建系统、`app/`、`diag/`、`tools/`）
- `sim/`（Proteus / Wokwi 仿真工程）
- `docs/`、`board/`、`README.md`、`CHANGELOG.md`、`scripts/`

## 4. 使用建议

- **学习、课设、毕设、个人实验**：可以自由使用、修改、再分发，保留本文件与原始版权头即可。
- **商业用途**：需先取得正点原子与各芯片厂商的授权；MIT 许可不能替代这些授权。
- 本项目**不是医疗器械**，心率/血氧结果与跌倒判定未经临床验证，不得用于任何诊断或监护场景。
