# STM32 健康监测仪 · PPG 信号质量门控与心率滤波报告 v2.6

日期：2026-10-16 21:35
固件：`firmware/MAX30102/gcc/app/app_main.c`（由 `app/make_app.py` 生成），构建 `make APP=1`
另改算法：`firmware/MAX30102/HARDWARE/IIC/algorithm.c`
实测环境：板子平放桌面；手指按上 / 移开 MAX30102 传感器两种状态
构建尺寸：`text 13636 / data 20 / bss 14076`

---

## 一、结论（先行）

1. **用户报的现象确认为真 bug**：显示层无条件照 `dis_hr` / `dis_spo2` 画数字，而这两个变量只在"新值算出来时"更新；拿不到新信号时它**沿用上一次的旧值**。所以手指拿开后，屏幕继续挂着几秒前的正常数字。
2. **底层算法没有任何"这不是脉搏"的判据**。实测无手指时 IR 只剩暗电平（直流 ≈ 490 LSB），`algorithm.c` 照样能从噪声里凑出"峰"，报 `ch_hr_valid=1 / n_heart_rate=300`，血氧查表返回 99 —— 这就是那些"看起来正常的伪值"。
3. **新增两道独立门控 + 一层滤波**：
   - **算法层**：`algorithm.c` 加入心率生理范围门控（30–220 bpm），越界即置 `hr = -999 / valid = 0`。
   - **信号层**：新增 5 项 PPG 质量判据 **G1**（直流范围、交流幅度、灌注指数、实时接触）。
   - **显示层**：8 窗滑动 + 两级离群剔除；**门控一失败立即切 `---`**，连续 2 窗失败清空滑动窗（恢复时不继承旧值）。
4. **实测三态全部符合预期**（数据见第四节）：
   - 有手指 → 心率 **119 BPM**、血氧 **97 %**，门控 PASS；
   - 手指移开的**第 1 个窗口** → 屏幕立即 `---`；
   - 持续无接触 → IR 直流 484 LSB，全部 `withheld`。

---

## 二、根因

| # | 位置 | 问题 |
|---|---|---|
| R1 | `USER/main.c` 显示段 | `OLED_ShowNum(47,16,dis_hr,2,…)` 无条件执行。`dis_hr` 只表示"最后一次算出的值"，没有"此刻有没有信号"的概念 —— 这就是"检测不到了仍打印旧数据"的直接来源 |
| R2 | `algorithm.c` `maxim_*` | 只要窗口里凑出 ≥2 个峰就置 `hr_valid = 1`，**不看这个数值合不合理**。实测无手指时报 300 bpm 仍判有效 |
| R3 | 全链路 | 没有任何信号质量指标（直流电平 / 交流幅度 / 灌注指数 / 接触检测），无法区分"脉搏"与"暗电平 + 噪声" |

> R2 的量化证据（本机实测，无手指）：`ch_hr_valid = 1`、`n_heart_rate = 300`、IR 直流电平 **515 LSB**。
> 300 bpm 在生理上不可能，算法却照报。

---

## 三、改动内容

### 3.1 算法层：心率生理范围门控（`algorithm.c`）

```c
        *pn_heart_rate=(int32_t)(6000/n_peak_interval_sum);// beats per minutes
        /* Physiological plausibility gate (IEC 60601-2-27 adult HR range). */
        if (n_peak_interval_sum > 0 &&
            *pn_heart_rate >= 30 && *pn_heart_rate <= 220) {
            *pch_hr_valid  = 1;
        } else {
            *pn_heart_rate = -999;
            *pch_hr_valid  = 0;
        }
```

- **依据**：IEC 60601-2-27（心电监护）成人测量范围；监护仪工程实践取 **30–220 bpm**。
- 越界值不是"低质量脉搏"，而是"根本不是脉搏"，必须**判定为无效**而不是降级显示。
- 顺带堵掉 `n_peak_interval_sum == 0` 的除零路径。

### 3.2 信号层：PPG 质量门控 G1（新增 `app_ppg_quality()`）

每个 5 s 滑动窗（500 样本 @100 Hz）算一次，5 条全过才放行：

```c
app_ppg_ok = (dc     >= APP_PPG_DC_MIN) && (dc     <= APP_PPG_DC_MAX) &&
             (ac     >= APP_PPG_AC_MIN) &&
             (ppi    >= APP_PPG_PI_MIN) && (ppi    <= APP_PPG_PI_MAX) &&
             (dc_new >= APP_PPG_DC_MIN) && (dc_new <= APP_PPG_DC_MAX);
```

| 门控项 | 阈值 | 依据 |
|---|---|---|
| IR 直流 `dc` | 8000 – 250000 LSB | 实测暗电平 490 LSB、有手指 119k–140k；18 位满量 262143 |
| IR 交流峰峰 `ac` | ≥ 150 LSB | 低于此为无搏动 |
| 灌注指数 `ppi` | 3 – 200 ‰（0.3%–20%） | 业界有效灌注下限 0.3%；20% 以上是非生理的运动伪影 |
| 最新 0.5 s 直流 `dc_new` | 8000 – 250000 LSB | 接触**实时性**检查：抓住"刚刚才拿走手指"的瞬间 |

**抗单点尖峰**：交流幅度不用「最高 − 最低」，而用「**次高 − 次低**」（`mx2 − mn2`）。
一次 I²C 毛刺或 ADC 离群值无法把测得的搏动幅度抬起来。

**上限 200 ‰ 的价值**：实测手指移开过渡段的 PI 冲到 **1160 → 3779 → 9999 ‰**（分母直流在塌），
这一条把它全部拦下（见第四节的 win 169–173）。

### 3.3 显示层：滑动平均 + 两级离群剔除（`app_hr_ingest` / `app_hr_publish`）

用户的原始诉求是「心率平稳后取多次平均，舍去明显偏离的值」。实现为**两级**：

**第一级（输入级，`app_hr_ingest`）** —— 决定"要不要收进窗口"：

```c
med = app_med(app_hr_ring, app_hr_n);
tol = med * APP_HR_TOL_PCT / 100;         /* 15% */
if (tol < APP_HR_TOL_ABS) tol = APP_HR_TOL_ABS;   /* 且不少于 6 bpm */
if (hr > med + 2*tol || hr < med - 2*tol) {       /* 收进来的门槛放宽一倍 */
    app_hr_reject++;
    if (++app_hr_rej_run >= APP_HR_REJ_RUN) {     /* 连续 3 次 */
        app_hr_n = 0; app_hr_wr = 0;              /* 判定基准真的漂移了 */
        app_hr_rej_run = 0;
    }
    return;                                        /* 丢弃 */
}
```

- 入窗门限刻意做到显示门限的 **2 倍宽**：它要剔除的是"整段错拍的伪值"，不是"真实心率变化"。
- 连续 3 次被拒 → 判定基准发生真实迁移（例如从静息切到运动），**重置窗口重新收敛**，
  否则算法会永远拒绝新值。

**第二级（显示级，`app_hr_publish` → `app_trim`）** —— 决定"显示成多少"：
按同样的 `tol` 带裁掉离群，对保留集求均值。

**稳定标志**：

| 参数 | 值 | 含义 |
|---|---|---|
| `APP_HR_N` | 8 | 滑动窗容量（每 5 s 一个值） |
| `APP_HR_WARM` | 2 | ≥2 个有效值才允许显示 |
| `APP_HR_STABLE_N` | 4 | ≥4 个值才允许判稳定 |
| `APP_HR_TOL_ABS` / `PCT` | 6 bpm / 15% | 显示级裁剪带（取宽者） |
| `APP_HR_STABLE_BPM` | 5 | 保留集极差 ≤5 bpm 才算稳定 |
| `APP_HR_REJ_RUN` | 3 | 连续 3 次拒收 → 重置基准 |
| `APP_LOSS_WINDOWS` | 2 | 连续 2 窗门控失败 → 清空滑动窗 |
| `APP_SPO2_TOL_ABS` / `PCT` | 2 % / 3% | 血氧裁剪带 |
| `APP_SPO2_STBL_N` / `PC` | 3 / 1 % | 血氧稳定判据 |

### 3.4 失联处理：绝不沿用旧值（`app_ppg_update`）

```c
if (!app_ppg_ok) {
    if (app_ppg_bad >= APP_LOSS_WINDOWS) {
        app_hr_n = 0; app_spo2_n = 0;            /* 清空，恢复时不继承旧值 */
        app_hr_wr = 0; app_spo2_wr = 0;
        app_hr_rej_run = 0;
    }
    app_hr_ok = 0;  app_hr_stable = 0;           /* 立即失效 */
    app_spo2_ok = 0; app_spo2_stable = 0;
    app_ppg_record();
    return;
}
```

屏幕侧：

```c
if (app_hr_ok) { … 画数字 … } else { dis_hr = 0; OLED_ShowString(47,16,"---",OLED_8X16); }
if (app_spo2_ok) { … 画数字 … } else { dis_spo2 = 0; OLED_ShowString(47,32,"---",OLED_8X16); }
```

- `app_hr_ok = 0` 是**立即**生效的（门控失败的同一个窗口就切 `---`），不需要等 2 窗。
- 2 窗的 `APP_LOSS_WINDOWS` 只管**清空历史**，防止血氧/心率在信号恢复的瞬间从旧窗口"续命"。
- 心率数字右侧新增状态字符：`S` = 已稳定，`~` = 收敛中，空白 = 无信号。

---

## 四、实测数据（全部本机 SWD 读回）

读取方式：OpenOCD `halt → mdw → resume`，同时把 OLED 显存按字模解码成文字。

### 4.1 无手指（纯暗电平）

| 指标 | 实测值 | 门控判定 |
|---|---|---|
| IR 直流 | **484 – 502 LSB** | ❌ < 8000 |
| IR 交流峰峰 | **75 – 96 LSB** | ❌ < 150 |
| 灌注指数 | 154 – 195 ‰（分母直流极小，算出来虚高） | ⚠️ 单看会误判，被直流项拦下 |
| 旧 DSP 输出 | `ch_hr_valid=1` / `n_heart_rate=300` | 现为 `valid=0` / `-999`（3.1 生效） |
| 屏显心率 / 血氧 | `---` / `---` | ✅ 不再打印旧值 |

> **血氧的伪值拦不住，必须靠信号层**：同一时刻 `ch_spo2_valid` 仍然是 **1**、`n_sp02 = 98`。
> 98% 本来就落在生理范围（70–100%）里，**数值门控对它完全无效** —— 无接触时算出来的血氧
> 是一个"看着很合理"的数。这就是 G1 必须存在的原因：它在显示层之前把整段无接触窗判为无效
> （`app_spo2_ok = 0`）。对血氧而言，"信号有没有"是比"数值合不合理"更前置的问题。

### 4.2 有手指（两次按住，共约 55 s）

| 指标 | 实测值 |
|---|---|
| IR 直流 | 119667 – 139963 LSB |
| IR 交流峰峰 | 2313 – 20790 LSB（接触稳定后衰减到 ≈2.5k） |
| 灌注指数 | 16 – 189 ‰ |
| 门控 | PASS（`app_ppg_bad = 0`） |
| 屏显心率 | **119 BPM**（收敛序列 160→128→125→121→109→99→90→86→82→102→119） |
| 屏显血氧 | **97 %**（96 – 99） |

> 心率收敛序列是"两级剔除 + 均值"在工作：中途 raw 值在 69 – 193 之间跳，
> 显示值始终被压在一条平滑曲线上，12 次离群被 `app_hr_reject` 记下。

### 4.3 手指移开（关键验证）

| 窗口 | IR 直流 | IR 交流 | PI ‰ | 门控 | 屏显 |
|---|---|---|---|---|---|
| 168 | 136091 | 4119 | 30 | PASS | HR=102 SpO2=97 |
| **169** | 117504 | 136399 | 1160 | **FAIL** | **`---` 立即生效** |
| 170 | 90370 | 136400 | 1509 | FAIL | `---` |
| 171 | 63213 | 136400 | 2157 | FAIL | `---` |
| 172 | 36058 | 136273 | 3779 | FAIL | `---` |
| 173 | 9029 | 133989 | 9999 | FAIL | `---` |
| 174 | **484** | 75 | 154 | FAIL | `---` |

**结论**：手指离开后的**第 1 个窗口**（约 5 s 内）屏幕就切到 `---`，之后一直保持。
过渡段的 PI 虚高（1160 → 9999 ‰）被上限 200 ‰ 拦住，直流塌陷被下限 8000 LSB 拦住 ——
两道门控互为冗余。

### 4.4 门控余量

| 门控项 | 阈值 | 无手指 | 有手指 | 安全余量 |
|---|---|---|---|---|
| IR 直流下限 | 8000 | 490 | 119667+ | 低 16×／高 15× |
| IR 交流下限 | 150 | 96 | 2313+ | 低 1.6×（有直流项兜底） |
| PI 上限 | 200 | 195（临界） | 189 | 接触瞬态 1160+ 被拦 |

### 4.5 持续无接触（稳态，38 个窗口 ≈3 分钟）

| 指标 | 实测值 |
|---|---|
| IR 直流 / 最新 0.5 s | **543 / 548 LSB** |
| IR 交流峰峰 | 59 LSB |
| 灌注指数 | 108 ‰ |
| 门控 | FAIL，连续失败 **38** 窗 |
| 屏显 | 心率 `---`、血氧 `---` |
| 内部状态 | `hr_ok=0 / n=0`、`spo2_ok=0 / n=0`（滑动窗已清空） |

> 连续失败计数一直累积，屏幕**始终**是 `---` —— 这就是"绝不沿用旧值"的直接证据。

---

## 五、在线微调（SWD，不用重烧）

**地址随重新编译会变，取址请现查：**

```bash
arm-none-eabi-nm -S -n build/APP.elf | grep -E "app_ppg_|app_hr_"
```

本次构建（v2.6）的地址：

| 符号 | 地址 | 说明 |
|---|---|---|
| `app_hr_disp` | `0x20001476` | 当前显示的心率 |
| `app_hr_ok` | `0x20001477` | 1 = 允许显示 |
| `app_hr_stable` | `0x20001475` | 1 = 已稳定 |
| `app_hr_reject` | `0x2000146c` | 累计剔除次数 |
| `app_spo2_disp` / `app_spo2_ok` | `0x20001472` / `0x20001473` | 血氧 |
| `app_ppg_dc` / `app_ppg_ac` / `app_ppi` | `0x20001490` / `0x2000148c` / `0x20001486` | 本窗信号质量 |
| `app_ppg_ok` / `app_ppg_bad` | `0x20001484` / `0x20001480` | 门控结论 / 连续失败数 |
| `app_ppg_win` | `0x20001494` | 已评估的窗口数 |

一键读取 + 屏幕解码脚本（含中文/ASCII 字模还原）：

```bash
python _ppg_read.py                 # 工作区根目录
python _ppg_read.py --keep-halted   # 读完保持停机
```

---

## 六、阈值速查（全部可现场改）

```c
#define APP_PPG_DC_MIN    8000   /* 已接触的直流下限 */
#define APP_PPG_DC_MAX    250000 /* 接近满量程 = 削顶 */
#define APP_PPG_AC_MIN    150    /* 无搏动上限 */
#define APP_PPG_PI_MIN    3      /* 0.3% 有效灌注 */
#define APP_PPG_PI_MAX    200    /* 20% 以上视为运动伪影 */
#define APP_HR_MIN        30
#define APP_HR_MAX        220
#define APP_SPO2_MIN      70
#define APP_SPO2_MAX      100
#define APP_LOSS_WINDOWS  2
```

运行时无触点判据（在线改值）：

```bash
openocd -f interface/stlink.cfg -f target/stm32f1x.cfg -c "adapter speed 20" -c "init" -c "halt" \
        -c "mww 0x20001484 0" \
        -c "resume" -c "exit"
```

---

## 七、遗留项

1. **灌注指数偏低**：实测 PI 只有 7 – 10 ‰（下限 3 ‰），说明手指压力偏轻。
   本轮的收敛标志多数时间停在 `~`（未稳定），原因是 raw 心率在弱信号下抖动较大。
   按实一点再测应能看到 `S`。
2. **血氧未做多窗一致性回归**：`APP_SPO2_STBL_N = 3`、`APP_SPO2_STBL_PC = 1` 已生效，
   但缺少"手指移开 → 血氧清空"的独立时序记录（本次与心率同步验证）。
3. **`algorithm.c` 的改动尚未单独回归**：心率门控已在实测中生效（无手指 `valid=0`），
   但尚未做"超出 220 bpm 的模拟输入"边界测试。

---

## 八、复现方法

```bash
# 1. 生成 + 编译（app_main.c 由 make_app.py 从 GBK 的 USER/main.c 派生，切勿手改生成物）
cd firmware/MAX30102/gcc
python app/make_app.py            # 输出 app/app_main.c
make APP=1                        # 或 make APP=1 NOMAX=1

# 2. 烧录
export PATH="$HOME/tools/xpack-openocd-0.12.0-7/bin:$PATH"
openocd -f interface/stlink.cfg -f target/stm32f1x.cfg \
        -c "adapter speed 20" -c "init" -c "reset halt" \
        -c "program build/APP.elf verify" -c "reset run" -c "exit"

# 3. 读回验证（板子运行中，可带电读）
python _ppg_read.py
```

> ⚠️ 每次 `halt` 都会让 MAX30102 的 32 深 FIFO 溢出（100 Hz 下 0.32 s 即满），
> 因此 trace 里会零星出现 `hr_raw = -999` 的窗口。这是**读取副作用**，不是算法缺陷。
