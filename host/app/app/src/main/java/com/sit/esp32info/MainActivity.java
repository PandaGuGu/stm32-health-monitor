package com.sit.esp32info;

import android.app.Activity;
import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Typeface;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.text.Editable;
import android.text.InputType;
import android.text.TextWatcher;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import org.json.JSONArray;
import org.json.JSONObject;

import java.util.LinkedHashMap;
import java.util.Map;

/**
 * ESP32 信息节点 · Android 上位机
 *
 * 数据全部来自 PC 网关（host/gateway）的 REST 接口，1 Hz 轮询：
 *   GET  /api/status   → 合并后的 snapshot
 *   POST /api/connect  → 切数据源（serial / wifi / mock）
 *   POST /api/cmd      → 下发命令
 */
public class MainActivity extends Activity {

    private static final int[] OUTPUT_PINS = {2, 4, 5, 12, 13, 14, 15, 16, 17, 18, 19, 21, 22, 23, 25, 26, 27, 32, 33};
    private static final int[] INPUT_ONLY = {34, 35, 36, 39};

    private final GatewayClient api = new GatewayClient();
    private final Handler ui = new Handler(Looper.getMainLooper());
    private final Map<String, PinCell> pins = new LinkedHashMap<>();

    private SharedPreferences prefs;
    private volatile boolean alive = true;
    private volatile String source = "mock";

    private View dot;
    private TextView tvTitleSub, tvState, tvHint, tvHeapTag, tvAck, tvLog, tvLogCount;
    private TextView segSerial, segWifi, segMock, segDirect;
    private EditText etGateway, etHost, etCmd;
    private LinearLayout wifiGroup;
    private Ui.KvGrid gDev, gNet, gRt, gVit;
    private SparkView spHeap, spTemp, spRssi, spBpm;
    private TextView tvVitTag;
    private LinearLayout pinBox, adcBox, scanBox;
    private String lastScanKey = "";
    private boolean sourceSynced;      // 首次连上网关时，把本地选择切到网关实际在用的源

    // ------------------------------------------------------------ 生命周期
    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        prefs = getSharedPreferences("esp32info", Context.MODE_PRIVATE);

        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        scroll.setBackgroundColor(Ui.BG);
        scroll.setFitsSystemWindows(true);   // 不压到状态栏

        LinearLayout root = Ui.col(this);
        int pad = Ui.dp(this, 13);
        root.setPadding(pad, pad, pad, Ui.dp(this, 24));

        root.addView(buildHeader(), Ui.matchWrap(Ui.dp(this, 12)));

        // 宽屏（MuMu 横屏 1600x900 ≈ sw600dp）走双列，窄屏单列往下排
        boolean wide = getResources().getConfiguration().screenWidthDp >= 700;
        if (wide) {
            LinearLayout cols = Ui.row(this);
            cols.setGravity(Gravity.TOP);

            LinearLayout left = Ui.col(this);
            left.addView(buildConnectCard(), Ui.matchWrap(Ui.dp(this, 12)));
            left.addView(buildVitalsCard(), Ui.matchWrap(Ui.dp(this, 12)));
            left.addView(buildRuntimeCard(), Ui.matchWrap(Ui.dp(this, 12)));
            left.addView(buildScanCard(), Ui.matchWrap(0));

            LinearLayout right = Ui.col(this);
            right.addView(buildDeviceCard(), Ui.matchWrap(Ui.dp(this, 12)));
            right.addView(buildIoCard(), Ui.matchWrap(Ui.dp(this, 12)));
            right.addView(buildNetCard(), Ui.matchWrap(0));

            LinearLayout.LayoutParams lp = Ui.weight1();
            lp.rightMargin = Ui.dp(this, 12);
            cols.addView(left, lp);
            cols.addView(right, Ui.weight1());
            root.addView(cols, Ui.matchWrap(Ui.dp(this, 12)));

            root.addView(buildCmdCard(), Ui.matchWrap(Ui.dp(this, 12)));
            root.addView(buildLogCard(), Ui.matchWrap(0));
        } else {
            root.addView(buildConnectCard(), Ui.matchWrap(Ui.dp(this, 12)));
            root.addView(buildVitalsCard(), Ui.matchWrap(Ui.dp(this, 12)));
            root.addView(buildDeviceCard(), Ui.matchWrap(Ui.dp(this, 12)));
            root.addView(buildNetCard(), Ui.matchWrap(Ui.dp(this, 12)));
            root.addView(buildRuntimeCard(), Ui.matchWrap(Ui.dp(this, 12)));
            root.addView(buildIoCard(), Ui.matchWrap(Ui.dp(this, 12)));
            root.addView(buildScanCard(), Ui.matchWrap(Ui.dp(this, 12)));
            root.addView(buildCmdCard(), Ui.matchWrap(Ui.dp(this, 12)));
            root.addView(buildLogCard(), Ui.matchWrap(0));
        }

        scroll.addView(root);
        setContentView(scroll);

        api.base = prefs.getString("gateway", "http://10.0.2.2:8080");
        etGateway.setText(api.base);
        etHost.setText(prefs.getString("host", "192.168.1.66"));
        setSource(prefs.getString("source", "mock"), false);

        // 允许 adb 直接指定：am start -e url http://192.168.4.1（直连设备时很方便）
        String launchUrl = getIntent() != null ? getIntent().getStringExtra("url") : null;
        if (launchUrl != null && !launchUrl.trim().isEmpty()) {
            api.base = launchUrl.trim();
            etGateway.setText(api.base);
            setSource("direct", true);
        }

        // 不让输入框抢焦点（否则一进页面就冒出光标手柄和键盘）
        scroll.setFocusableInTouchMode(true);
        etGateway.clearFocus();
        etHost.clearFocus();
        etCmd.clearFocus();
        scroll.requestFocus();

        startPolling();
    }

    @Override
    protected void onDestroy() {
        alive = false;
        super.onDestroy();
    }

    // ------------------------------------------------------------ 头部
    private View buildHeader() {
        LinearLayout row = Ui.row(this);

        LinearLayout left = Ui.col(this);
        TextView title = Ui.text(this, "ESP32 信息节点", 18, Ui.TEXT, false);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        tvTitleSub = Ui.text(this, "上位机 · 未连接", 11, Ui.MUTED, false);
        left.addView(title);
        left.addView(tvTitleSub);

        LinearLayout right = Ui.row(this);
        dot = new View(this);
        LinearLayout.LayoutParams dlp = new LinearLayout.LayoutParams(Ui.dp(this, 9), Ui.dp(this, 9));
        dlp.rightMargin = Ui.dp(this, 6);
        dot.setBackground(Ui.bg(Ui.DIM, 0, 9, this));
        right.addView(dot, dlp);

        tvState = Ui.text(this, "离线", 11, Ui.MUTED, true);
        tvState.setPadding(Ui.dp(this, 7), Ui.dp(this, 3), Ui.dp(this, 7), Ui.dp(this, 3));
        tvState.setBackground(Ui.bg(Ui.CARD_2, Ui.LINE, 20, this));
        right.addView(tvState);

        row.addView(left, Ui.weight1());
        row.addView(right);
        return row;
    }

    // ------------------------------------------------------------ 连接栏
    private View buildConnectCard() {
        LinearLayout body = Ui.col(this);

        LinearLayout seg = Ui.row(this);
        seg.setBackground(Ui.bg(Ui.CARD_2, Ui.LINE, 9, this));
        segSerial = segButton("串口");
        segWifi = segButton("WiFi");
        segMock = segButton("模拟器");
        segDirect = segButton("直连");
        seg.addView(segSerial, Ui.weight1());
        seg.addView(segWifi, Ui.weight1());
        seg.addView(segMock, Ui.weight1());
        seg.addView(segDirect, Ui.weight1());
        segSerial.setOnClickListener(v -> setSource("serial", true));
        segWifi.setOnClickListener(v -> setSource("wifi", true));
        segMock.setOnClickListener(v -> setSource("mock", true));
        segDirect.setOnClickListener(v -> setSource("direct", true));
        body.addView(seg, Ui.matchWrap(Ui.dp(this, 10)));

        etGateway = input("http://10.0.2.2:8080", InputType.TYPE_TEXT_VARIATION_URI);
        body.addView(label("网关地址"), Ui.matchWrap(0));
        body.addView(etGateway, Ui.matchWrap(Ui.dp(this, 8)));
        etGateway.addTextChangedListener(new TextWatcher() {
            public void beforeTextChanged(CharSequence s, int a, int b, int c) {}
            public void onTextChanged(CharSequence s, int a, int b, int c) {}
            public void afterTextChanged(Editable s) { api.base = s.toString(); }
        });

        etHost = input("192.168.1.66", InputType.TYPE_TEXT_VARIATION_URI);
        wifiGroup = Ui.col(this);
        wifiGroup.addView(label("设备地址（WiFi 源）"), Ui.matchWrap(0));
        wifiGroup.addView(etHost, Ui.matchWrap(Ui.dp(this, 8)));
        wifiGroup.setVisibility(View.GONE);
        body.addView(wifiGroup);

        LinearLayout btns = Ui.row(this);
        Button btnConnect = new Button(this);
        btnConnect.setText("连接");
        btnConnect.setAllCaps(false);
        btnConnect.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        btnConnect.setTextColor(0xFFFFFFFF);
        btnConnect.setBackground(Ui.bg(0xFF2F7FE0, 0, 9, this));
        btnConnect.setOnClickListener(v -> connect());
        btns.addView(btnConnect, Ui.weight1());

        Button btnDisconnect = new Button(this);
        btnDisconnect.setText("断开");
        btnDisconnect.setAllCaps(false);
        btnDisconnect.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        btnDisconnect.setTextColor(Ui.TEXT);
        btnDisconnect.setBackground(Ui.bg(Ui.CARD_2, Ui.LINE, 9, this));
        btnDisconnect.setOnClickListener(v -> {
            new Thread(() -> {
                try { api.post("/api/disconnect", new JSONObject(), 8000); } catch (Exception ignored) {}
            }).start();
            tvHint.setText("已请求断开");
        });
        LinearLayout.LayoutParams blp = Ui.weight1();
        blp.leftMargin = Ui.dp(this, 8);
        btns.addView(btnDisconnect, blp);
        body.addView(btns, Ui.matchWrap(Ui.dp(this, 8)));

        tvHint = Ui.text(this, "选「模拟器」可无硬件先跑通全流程", 10.5f, Ui.DIM, false);
        body.addView(tvHint);

        return Ui.card(this, "数据源", body, null);
    }

    private TextView segButton(String label) {
        TextView t = Ui.text(this, label, 12.5f, Ui.MUTED, false);
        t.setGravity(Gravity.CENTER);
        t.setPadding(0, Ui.dp(this, 8), 0, Ui.dp(this, 8));
        return t;
    }

    private TextView label(String s) {
        TextView t = Ui.text(this, s, 10, Ui.DIM, false);
        t.setPadding(0, 0, 0, Ui.dp(this, 3));
        return t;
    }

    private EditText input(String hint, int variation) {
        EditText e = new EditText(this);
        e.setHint(hint);
        e.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12.5f);
        e.setTextColor(Ui.TEXT);
        e.setHintTextColor(Ui.DIM);
        e.setTypeface(Typeface.MONOSPACE);
        e.setSingleLine(true);
        e.setInputType(InputType.TYPE_CLASS_TEXT | variation);
        e.setBackground(Ui.bg(Ui.CARD_2, Ui.LINE, 9, this));
        e.setPadding(Ui.dp(this, 10), Ui.dp(this, 8), Ui.dp(this, 10), Ui.dp(this, 8));
        return e;
    }

    private void setSource(String src, boolean save) {
        source = src;
        int active = 0xFF16324F;
        segSerial.setBackground(Ui.bg("serial".equals(src) ? active : 0x00000000, 0, 8, this));
        segSerial.setTextColor("serial".equals(src) ? Ui.ACCENT : Ui.MUTED);
        segWifi.setBackground(Ui.bg("wifi".equals(src) ? active : 0x00000000, 0, 8, this));
        segWifi.setTextColor("wifi".equals(src) ? Ui.ACCENT : Ui.MUTED);
        segMock.setBackground(Ui.bg("mock".equals(src) ? active : 0x00000000, 0, 8, this));
        segMock.setTextColor("mock".equals(src) ? Ui.ACCENT : Ui.MUTED);
        segDirect.setBackground(Ui.bg("direct".equals(src) ? active : 0x00000000, 0, 8, this));
        segDirect.setTextColor("direct".equals(src) ? Ui.ACCENT : Ui.MUTED);
        if (etHost != null) etHost.setVisibility("wifi".equals(src) ? View.VISIBLE : View.GONE);
        if (wifiGroup != null) wifiGroup.setVisibility("wifi".equals(src) ? View.VISIBLE : View.GONE);
        if (save) prefs.edit().putString("source", src).apply();
    }

    private void connect() {
        setSource(source, true);
        prefs.edit().putString("gateway", etGateway.getText().toString())
                .putString("host", etHost.getText().toString()).apply();
        final String src = source;
        final String host = etHost.getText().toString().trim();
        if ("direct".equals(src)) {
            api.base = etGateway.getText().toString().trim();
            tvHint.setText("直连设备 " + api.base + "（不走 PC 网关）");
            return;
        }
        tvHint.setText("正在连接 " + src + " …");
        new Thread(() -> {
            try {
                JSONObject body = new JSONObject().put("source", src);
                if ("wifi".equals(src)) body.put("host", host);
                if ("serial".equals(src)) {
                    JSONObject ports = api.get("/api/ports", 5000);
                    JSONArray arr = ports.optJSONArray("ports");
                    if (arr == null || arr.length() == 0) {
                        postHint("电脑上没发现串口：先插 ESP32，或改用 WiFi / 模拟器");
                        return;
                    }
                    String port = arr.getJSONObject(0).optString("device");
                    for (int i = 0; i < arr.length(); i++) {
                        JSONObject p = arr.getJSONObject(i);
                        if (p.optBoolean("likely_esp32")) { port = p.optString("device"); break; }
                    }
                    body.put("port", port);
                }
                JSONObject r = api.post("/api/connect", body, 15000);
                postHint("已连接：" + r.optString("source") + " " + r.optString("port", r.optString("host", "")));
            } catch (Exception e) {
                postHint("连接失败：" + e.getMessage());
            }
        }).start();
    }

    private void postHint(String s) {
        ui.post(() -> tvHint.setText(s));
    }

    // ------------------------------------------------------------ 卡片
    private View buildDeviceCard() {
        gDev = new Ui.KvGrid(this, 2);
        gDev.add(this, "model", "型号");
        gDev.add(this, "fw", "固件");
        gDev.add(this, "build", "编译时间");
        gDev.add(this, "cores", "核心 / 修订");
        gDev.add(this, "freq", "主频");
        gDev.add(this, "flash", "Flash");
        gDev.add(this, "sdk", "SDK");
        gDev.add(this, "mac", "MAC");
        gDev.add(this, "chipid", "Chip ID");
        gDev.add(this, "reset", "复位原因");
        return Ui.card(this, "芯片与固件", gDev.root, null);
    }

    private View buildNetCard() {
        gNet = new Ui.KvGrid(this, 2);
        gNet.add(this, "mode", "模式 / 状态");
        gNet.add(this, "ssid", "SSID");
        gNet.add(this, "ip", "IP");
        gNet.add(this, "gw", "网关");
        gNet.add(this, "mask", "掩码");
        gNet.add(this, "ch", "信道");
        gNet.add(this, "rssi", "RSSI");
        gNet.add(this, "host", "主机名");
        return Ui.card(this, "网络", gNet.root, null);
    }

    /** 生理数据：STM32 → ESP32 UART2 → 网关；没接 STM32 时显示"未接入" */
    private View buildVitalsCard() {
        LinearLayout body = Ui.col(this);

        gVit = new Ui.KvGrid(this, 2);
        gVit.add(this, "bpm", "心率");
        gVit.add(this, "spo2", "血氧");
        gVit.add(this, "temp", "体温");
        gVit.add(this, "fall", "跌倒");
        gVit.add(this, "age", "帧龄");
        gVit.add(this, "src", "来源 / 帧数");
        body.addView(gVit.root);

        LinearLayout sparks = Ui.row(this);
        sparks.setPadding(0, Ui.dp(this, 8), 0, 0);
        spBpm = addSpark(sparks, "心率趋势", "bpm");
        body.addView(sparks);

        tvVitTag = Ui.text(this, "未接入", 10.5f, Ui.MUTED, true);
        tvVitTag.setPadding(Ui.dp(this, 7), Ui.dp(this, 2), Ui.dp(this, 7), Ui.dp(this, 2));
        tvVitTag.setBackground(Ui.bg(Ui.CARD_2, Ui.LINE, 20, this));
        return Ui.card(this, "生理数据 · STM32", body, tvVitTag);
    }

    private View buildRuntimeCard() {        LinearLayout body = Ui.col(this);

        gRt = new Ui.KvGrid(this, 3);
        gRt.add(this, "up", "运行时长");
        gRt.add(this, "heap", "空闲堆");
        gRt.add(this, "heapmin", "历史最低");
        gRt.add(this, "frag", "碎片度");
        gRt.add(this, "temp", "芯片温度");
        gRt.add(this, "tasks", "任务数");
        body.addView(gRt.root);

        LinearLayout sparks = Ui.row(this);
        sparks.setPadding(0, Ui.dp(this, 10), 0, 0);
        spHeap = addSpark(sparks, "空闲堆", "0");
        spTemp = addSpark(sparks, "温度", "1");
        spRssi = addSpark(sparks, "RSSI", "2");
        body.addView(sparks);

        tvHeapTag = Ui.text(this, "—", 10.5f, Ui.MUTED, true);
        tvHeapTag.setPadding(Ui.dp(this, 7), Ui.dp(this, 2), Ui.dp(this, 7), Ui.dp(this, 2));
        tvHeapTag.setBackground(Ui.bg(Ui.CARD_2, Ui.LINE, 20, this));
        return Ui.card(this, "运行时资源", body, tvHeapTag);
    }

    private SparkView addSpark(LinearLayout parent, String caption, String tag) {
        LinearLayout box = Ui.col(this);
        TextView c = Ui.text(this, caption, 9.5f, Ui.DIM, false);
        SparkView sp = new SparkView(this);
        sp.setTag(tag);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, Ui.dp(this, 40));
        lp.topMargin = Ui.dp(this, 2);
        box.addView(c);
        box.addView(sp, lp);
        LinearLayout.LayoutParams blp = Ui.weight1();
        blp.rightMargin = Ui.dp(this, 10);
        parent.addView(box, blp);
        return sp;
    }

    private View buildIoCard() {
        LinearLayout body = Ui.col(this);
        pinBox = Ui.col(this);
        body.addView(pinBox);
        adcBox = Ui.col(this);
        adcBox.setPadding(0, Ui.dp(this, 8), 0, 0);
        body.addView(adcBox);
        TextView hint = Ui.text(this, "可输出引脚点击即可翻转电平", 9.5f, Ui.DIM, false);
        hint.setPadding(0, Ui.dp(this, 8), 0, 0);
        body.addView(hint);
        return Ui.card(this, "板载 IO", body, null);
    }

    private void ensurePins(JSONObject gpio) {
        if (!pins.isEmpty()) return;
        LinearLayout row = null;
        int inRow = 0;
        for (String key : sortedKeys(gpio)) {
            if (row == null || inRow == 4) {
                row = Ui.row(this);
                row.setPadding(0, 0, 0, Ui.dp(this, 6));
                pinBox.addView(row);
                inRow = 0;
            }
            final int pin = Integer.parseInt(key);
            boolean inputOnly = isInputOnly(pin);

            LinearLayout cell = Ui.col(this);
            cell.setBackground(Ui.bg(Ui.CARD_2, Ui.LINE, 8, this));
            int p = Ui.dp(this, 7);
            cell.setPadding(p, p, p, p);

            TextView name = Ui.text(this, "GPIO " + pin + (inputOnly ? " ·in" : ""), 9.5f, Ui.MUTED, true);
            TextView lvl = Ui.text(this, "LOW", 13, Ui.DIM, true);
            lvl.setTypeface(Typeface.MONOSPACE, Typeface.BOLD);
            cell.addView(name);
            cell.addView(lvl);

            if (!inputOnly) {
                cell.setOnClickListener(v -> {
                    PinCell pc = pins.get(String.valueOf(pin));
                    int cur = pc != null && pc.high ? 1 : 0;
                    sendCmd("gpio.set " + pin + " " + (cur == 1 ? 0 : 1), true);
                });
            }

            LinearLayout.LayoutParams lp = Ui.weight1();
            lp.rightMargin = Ui.dp(this, 6);
            row.addView(cell, lp);
            pins.put(key, new PinCell(cell, lvl));
            inRow++;
        }
        // 末行不足 4 个时补占位，保证每格等宽
        if (row != null && inRow > 0 && inRow < 4) {
            for (int i = inRow; i < 4; i++) {
                LinearLayout.LayoutParams lp = Ui.weight1();
                lp.rightMargin = Ui.dp(this, 6);
                row.addView(new View(this), lp);
            }
        }
    }

    private boolean isInputOnly(int pin) {
        for (int p : INPUT_ONLY) if (p == pin) return true;
        return false;
    }

    private View buildScanCard() {
        LinearLayout body = Ui.col(this);
        scanBox = Ui.col(this);
        body.addView(scanBox);
        TextView empty = Ui.text(this, "点右上角「扫描」", 11, Ui.DIM, false);
        empty.setPadding(0, Ui.dp(this, 6), 0, Ui.dp(this, 6));
        scanBox.addView(empty);

        TextView btn = Ui.text(this, "扫描", 11.5f, Ui.ACCENT, true);
        btn.setPadding(Ui.dp(this, 10), Ui.dp(this, 4), Ui.dp(this, 10), Ui.dp(this, 4));
        btn.setBackground(Ui.bg(0xFF16324F, 0, 20, this));
        btn.setOnClickListener(v -> sendCmd("scan", true));
        return Ui.card(this, "WiFi 扫描", body, btn);
    }

    private View buildCmdCard() {
        LinearLayout body = Ui.col(this);

        LinearLayout row = Ui.row(this);
        etCmd = input("命令，例如 scan / gpio.set 2 1", InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        row.addView(etCmd, Ui.weight1());
        Button send = new Button(this);
        send.setText("发送");
        send.setAllCaps(false);
        send.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        send.setTextColor(Ui.TEXT);
        send.setBackground(Ui.bg(0xFF16324F, 0, 9, this));
        send.setOnClickListener(v -> {
            String c = etCmd.getText().toString().trim();
            if (!c.isEmpty()) { sendCmd(c, false); etCmd.setText(""); }
        });
        LinearLayout.LayoutParams blp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        blp.leftMargin = Ui.dp(this, 8);
        row.addView(send, blp);
        body.addView(row, Ui.matchWrap(Ui.dp(this, 8)));

        tvAck = Ui.text(this, "—", 10.5f, Ui.MUTED, true);
        tvAck.setBackground(Ui.bg(0xFF080B11, Ui.LINE, 8, this));
        int p = Ui.dp(this, 9);
        tvAck.setPadding(p, p, p, p);
        tvAck.setTextIsSelectable(true);
        body.addView(tvAck);
        return Ui.card(this, "命令台", body, null);
    }

    private View buildLogCard() {
        tvLog = Ui.text(this, "等待网关数据…", 9.5f, 0xFFB9C6D6, true);
        tvLog.setBackground(Ui.bg(0xFF080B11, Ui.LINE, 8, this));
        int p = Ui.dp(this, 9);
        tvLog.setPadding(p, p, p, p);
        tvLog.setTextIsSelectable(true);

        ScrollView box = new ScrollView(this);
        box.addView(tvLog);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, Ui.dp(this, 190));
        box.setLayoutParams(lp);

        TextView cnt = Ui.text(this, "0 行", 10.5f, Ui.DIM, true);
        tvLogCount = cnt;
        return Ui.card(this, "原始数据流", box, cnt);
    }

    // ------------------------------------------------------------ 渲染
    private void render(JSONObject s) {
        boolean conn = s.optBoolean("connected");
        String src = s.optString("source", "none");

        dot.setBackground(Ui.bg(conn ? Ui.OK : ("none".equals(src) ? Ui.DIM : Ui.ERR), 0, 9, this));
        tvState.setText(conn ? "已连接" : ("none".equals(src) ? "未连接" : "等待数据"));
        tvState.setTextColor(conn ? Ui.OK : Ui.WARN);
        tvState.setBackground(Ui.bg(Ui.CARD_2, conn ? 0xFF2A5A34 : Ui.LINE, 20, this));

        String[] srcName = {"serial", "wifi", "mock"};
        String local = "device".equals(src) ? "direct" : src;
        String srcCn = "serial".equals(src) ? "串口" : "wifi".equals(src) ? "WiFi"
                : "mock".equals(src) ? "模拟器" : "device".equals(src) ? "直连设备" : "未连接";
        String detail = "serial".equals(src) ? s.optString("port", "") : "wifi".equals(src) ? s.optString("host", "") : "";
        tvTitleSub.setText("上位机 · " + srcCn + (detail.isEmpty() ? "" : " " + detail));

        // 网关可能已被别处（Web/命令行）切成别源，第一次拿到状态时对齐一下选中态
        if (!sourceSynced && !"none".equals(src)) {
            sourceSynced = true;
            if (!local.equals(source)) setSource(local, false);
        }

        JSONObject dev = s.optJSONObject("device");
        if (dev != null && dev.length() > 0) {
            gDev.put("model", dev.optString("model", "—"));
            gDev.put("fw", dev.optString("fw", "—"));
            gDev.put("build", dev.optString("build", "—"), Ui.MUTED);
            String cores = dev.has("cores") ? dev.optInt("cores") + " 核 · rev " + dev.optInt("rev") : "—";
            gDev.put("cores", cores);
            gDev.put("freq", dev.has("freq") ? dev.optInt("freq") + " MHz" : "—");
            gDev.put("flash", dev.has("flash_kb") ? (dev.optInt("flash_kb") / 1024) + " MB" : "—");
            gDev.put("sdk", dev.optString("sdk", "—"), Ui.MUTED);
            gDev.put("mac", dev.optString("mac", "—"));
            gDev.put("chipid", dev.optString("chip_id", "—"), Ui.MUTED);
            String reset = dev.optString("reset", "");
            gDev.put("reset", reset.isEmpty() ? "—" : reset,
                    "poweron".equals(reset) ? Ui.OK : reset.isEmpty() ? Ui.MUTED : Ui.WARN);
        }

        JSONObject net = s.optJSONObject("net");
        if (net != null && net.length() > 0) {
            gNet.put("mode", net.optString("mode", "—").toUpperCase() + " · " + net.optString("state", "—"));
            gNet.put("ssid", net.optString("ssid", "—"));
            gNet.put("ip", net.optString("ip", "—"));
            gNet.put("gw", net.optString("gw", "—"), Ui.MUTED);
            gNet.put("mask", net.optString("mask", "—"), Ui.MUTED);
            gNet.put("ch", String.valueOf(net.optInt("ch", 0)), Ui.MUTED);
            int rssi = net.optInt("rssi", 0);
            gNet.put("rssi", rssi == 0 ? "—" : rssi + " dBm",
                    rssi >= -60 ? Ui.OK : rssi >= -72 ? Ui.WARN : Ui.ERR);
            gNet.put("host", net.optString("host", "—"), Ui.MUTED);
        }

        JSONObject rt = s.optJSONObject("runtime");
        if (rt != null && rt.length() > 0) {
            int heap = rt.optInt("heap", 0);
            gRt.put("up", fmtUptime(rt.optLong("up", 0)));
            gRt.put("heap", fmtBytes(heap), heap >= 150000 ? Ui.OK : heap >= 80000 ? Ui.WARN : Ui.ERR);
            gRt.put("heapmin", fmtBytes(rt.optInt("heap_min", 0)), Ui.MUTED);
            int frag = rt.optInt("frag", 0);
            // ESP32 开 WiFi 后 frag 常态 ~55%（最大连续块 ≈110KB / 空闲堆 ≈250KB），阈值不能按 0 起算
            gRt.put("frag", frag + " %", frag <= 65 ? Ui.OK : frag <= 80 ? Ui.WARN : Ui.ERR);
            gRt.put("temp", rt.has("temp") ? rt.optDouble("temp", 0) + " ℃" : "—");
            gRt.put("tasks", String.valueOf(rt.optInt("tasks", 0)));
            tvHeapTag.setText(fmtBytes(heap));
            tvHeapTag.setTextColor(heap >= 150000 ? Ui.OK : heap >= 80000 ? Ui.WARN : Ui.ERR);
        }

        JSONObject vit = s.optJSONObject("vitals");
        if (vit != null) renderVitals(vit);

        JSONArray series = s.optJSONArray("series");
        drawSeries(series);

        JSONObject io = s.optJSONObject("io");
        if (io != null) {
            JSONObject gpio = io.optJSONObject("gpio");
            if (gpio != null && gpio.length() > 0) {
                ensurePins(gpio);
                for (String k : pins.keySet()) {
                    PinCell pc = pins.get(k);
                    boolean high = gpio.optInt(k, 0) == 1;
                    pc.high = high;
                    pc.lvl.setText(high ? "HIGH" : "LOW");
                    pc.lvl.setTextColor(high ? Ui.GREEN : Ui.DIM);
                    pc.root.setBackground(Ui.bg(high ? 0xFF12241B : Ui.CARD_2,
                            high ? 0xFF2E5A3C : Ui.LINE, 8, MainActivity.this));
                }
            }
            JSONObject adc = io.optJSONObject("adc");
            renderAdc(adc);
        }

        JSONObject scan = s.optJSONObject("scan");
        if (scan != null && scan.length() > 0) {
            String key = scan.optLong("ts", 0) + "/" + scan.optInt("n", 0);
            if (!key.equals(lastScanKey)) {
                lastScanKey = key;
                renderScan(scan);
            }
        }

        JSONArray raw = s.optJSONArray("raw");
        if (raw != null) renderRaw(raw, s.optJSONObject("stats"), src);
    }

    private void renderVitals(JSONObject v) {
        boolean ok = v.optBoolean("ok");
        String src = v.optString("src", "");
        int frames = v.optInt("frames", 0);

        if (!ok) {
            tvVitTag.setText(frames > 0 ? "已过期" : "未接入");
            tvVitTag.setTextColor(Ui.DIM);
            tvVitTag.setBackground(Ui.bg(Ui.CARD_2, Ui.LINE, 20, this));
            gVit.put("bpm", "—", Ui.DIM);
            gVit.put("spo2", "—", Ui.DIM);
            gVit.put("temp", "—", Ui.DIM);
            gVit.put("fall", "—", Ui.DIM);
            gVit.put("age", "—", Ui.DIM);
            gVit.put("src", frames > 0 ? "无有效帧（坏帧 " + v.optInt("bad", 0) + "）" : "等待 STM32", Ui.DIM);
            return;
        }

        boolean sim = "sim".equals(src);
        tvVitTag.setText(sim ? "模拟帧" : "STM32 在线");
        tvVitTag.setTextColor(sim ? Ui.WARN : Ui.OK);
        tvVitTag.setBackground(Ui.bg(Ui.CARD_2, sim ? 0x665A3F12 : 0x332A5A34, 20, this));

        int bpm = v.optInt("bpm", 0);
        int spo2 = v.optInt("spo2", 0);
        boolean fall = v.optInt("fall", 0) == 1;
        double temp = v.optDouble("temp", 0);
        long age = v.optLong("age_ms", 0);

        gVit.put("bpm", bpm + " BPM",
                bpm >= 60 && bpm <= 100 ? Ui.OK : bpm >= 50 && bpm <= 120 ? Ui.WARN : Ui.ERR);
        gVit.put("spo2", spo2 + " %", spo2 >= 95 ? Ui.OK : spo2 >= 90 ? Ui.WARN : Ui.ERR);
        gVit.put("temp", String.format(java.util.Locale.US, "%.1f ℃", temp), Ui.TEXT);
        gVit.put("fall", fall ? "是" : "否", fall ? Ui.ERR : Ui.OK);
        gVit.put("age", (age / 1000) + " s 前", age < 3000 ? Ui.MUTED : Ui.WARN);
        gVit.put("src", src + " · " + frames, Ui.MUTED);
    }

    private void drawSeries(JSONArray series) {
        if (series == null) return;
        int n = series.length();
        float[] heap = new float[n], temp = new float[n], rssi = new float[n], bpm = new float[n];
        int hn = 0, tn = 0, rn = 0, bn = 0;
        for (int i = 0; i < n; i++) {
            JSONObject p = series.optJSONObject(i);
            if (p == null) continue;
            if (p.has("heap")) heap[hn++] = (float) p.optDouble("heap", 0);
            if (p.has("temp")) temp[tn++] = (float) p.optDouble("temp", 0);
            if (p.has("rssi")) rssi[rn++] = (float) p.optDouble("rssi", 0);
            if (p.has("bpm")) bpm[bn++] = (float) p.optDouble("bpm", 0);
        }
        spHeap.setData(copyOf(heap, hn), Ui.ACCENT);
        spTemp.setData(copyOf(temp, tn), 0xFFFFA657);
        spRssi.setData(copyOf(rssi, rn), Ui.GREEN);
        if (spBpm != null) spBpm.setData(copyOf(bpm, bn), 0xFFFF7B72);
    }

    private float[] copyOf(float[] a, int n) {
        float[] r = new float[n];
        System.arraycopy(a, 0, r, 0, n);
        return r;
    }

    private void renderAdc(JSONObject adc) {
        adcBox.removeAllViews();
        if (adc == null || adc.length() == 0) return;
        for (String k : sortedKeys(adc)) {
            JSONObject o = adc.optJSONObject(k);
            int raw = o != null ? o.optInt("raw", 0) : adc.optInt(k, 0);
            int mv = o != null ? o.optInt("mv", 0) : 0;
            String text = "ADC1 · GPIO " + k + "    " + raw + " / 4095" + (mv > 0 ? "   " + mv + " mV" : "");
            TextView t = Ui.text(this, text, 11, 0xFF9FB0C4, true);
            t.setBackground(Ui.bg(Ui.CARD_2, Ui.LINE, 8, this));
            int p = Ui.dp(this, 7);
            t.setPadding(p, p, p, p);
            LinearLayout.LayoutParams lp = Ui.matchWrap(Ui.dp(this, 5));
            adcBox.addView(t, lp);
        }
    }

    private void renderScan(JSONObject scan) {
        scanBox.removeAllViews();
        JSONArray aps = scan.optJSONArray("aps");
        if (aps == null || aps.length() == 0) {
            TextView t = Ui.text(this, "未扫到 AP", 11, Ui.DIM, false);
            scanBox.addView(t);
            return;
        }
        for (int i = 0; i < aps.length(); i++) {
            JSONObject a = aps.optJSONObject(i);
            if (a == null) continue;
            LinearLayout row = Ui.row(this);
            row.setPadding(0, Ui.dp(this, 3), 0, Ui.dp(this, 3));

            TextView ssid = Ui.text(this, a.optString("ssid", "").isEmpty() ? "<hidden>" : a.optString("ssid"), 11.5f, Ui.TEXT, false);
            ssid.setSingleLine(true);
            ssid.setEllipsize(android.text.TextUtils.TruncateAt.END);
            row.addView(ssid, Ui.weight1());

            int rssi = a.optInt("rssi", 0);
            TextView rv = Ui.text(this, rssi + "", 11.5f,
                    rssi >= -60 ? Ui.OK : rssi >= -72 ? Ui.WARN : Ui.ERR, true);
            row.addView(rv);

            TextView ch = Ui.text(this, "  ch" + a.optInt("ch", 0), 10.5f, Ui.DIM, true);
            row.addView(ch);

            TextView enc = Ui.text(this, "  " + shortEnc(a.optString("enc", "")), 10, Ui.DIM, true);
            row.addView(enc);

            scanBox.addView(row);
        }
    }

    private String shortEnc(String e) {
        return e.replace("_PSK", "").replace("WPA_", "").replace("WPA2WPA3", "WPA2/3");
    }

    private void renderRaw(JSONArray raw, JSONObject stats, String src) {
        StringBuilder sb = new StringBuilder();
        int from = Math.max(0, raw.length() - 12);
        for (int i = from; i < raw.length(); i++) {
            JSONObject row = raw.optJSONObject(i);
            if (row == null) continue;
            Object d = row.opt("data");
            String line = d instanceof JSONObject ? d.toString() : String.valueOf(d);
            if (line.length() > 150) line = line.substring(0, 150) + "…";
            sb.append(line).append('\n');
        }
        boolean direct = "device".equals(src);
        tvLog.setText(sb.length() == 0
                ? (direct ? "直连模式：设备不缓存原始流，全量日志请用 PC 网关" : "等待网关数据…")
                : sb.toString());
        if (tvLogCount != null && stats != null) {
            tvLogCount.setText(stats.optInt("lines", 0) + " 行 · 错误 " + stats.optInt("errors", 0));
        }
    }

    // ------------------------------------------------------------ 命令
    private void sendCmd(String cmd, boolean compact) {
        if (!compact) tvAck.setText("→ " + cmd + "\n等待设备…");
        new Thread(() -> {
            try {
                JSONObject body = new JSONObject().put("cmd", cmd);
                if (cmd.startsWith("scan")) body.put("timeout", 9);
                JSONObject r = api.post("/api/cmd", body, 15000);
                JSONObject ack = r.optJSONObject("ack");
                String head;
                if (ack == null) head = "（无 ack）";
                else if ("err".equals(ack.optString("t"))) head = "✗ " + ack.optString("code") + "：" + ack.optString("msg");
                else head = "✓ ack";
                String detail = compact && ack != null ? ack.opt("data") + "" : (ack != null ? ack.toString(2) : r.toString());
                final String out = "→ " + cmd + "\n" + head + "\n" + detail;
                ui.post(() -> tvAck.setText(out));
            } catch (Exception e) {
                ui.post(() -> tvAck.setText("→ " + cmd + "\n失败：" + e.getMessage()));
            }
        }).start();
    }

    // ------------------------------------------------------------ 轮询
    private void startPolling() {
        Thread t = new Thread(() -> {
            while (alive) {
                try {
                    JSONObject s = api.get("/api/status", 4000);
                    final JSONObject snap = s;
                    ui.post(() -> render(snap));
                } catch (Exception e) {
                    final String msg = e.getMessage();
                    ui.post(() -> {
                        dot.setBackground(Ui.bg(Ui.ERR, 0, 9, MainActivity.this));
                        tvState.setText("网关离线");
                        tvState.setTextColor(Ui.ERR);
                        tvTitleSub.setText("上位机 · 连接 " + api.base + " 失败");
                        if (tvHint != null && tvHint.getText().toString().isEmpty()) {
                            tvHint.setText("网关离线：" + msg);
                        }
                    });
                }
                try { Thread.sleep(1000); } catch (InterruptedException ignored) { return; }
            }
        }, "gateway-poller");
        t.setDaemon(true);
        t.start();
    }

    // ------------------------------------------------------------ 小工具
    private java.util.List<String> sortedKeys(JSONObject o) {
        java.util.List<String> keys = new java.util.ArrayList<>();
        java.util.Iterator<String> it = o.keys();
        while (it.hasNext()) keys.add(it.next());
        keys.sort((a, b) -> {
            try { return Integer.compare(Integer.parseInt(a), Integer.parseInt(b)); }
            catch (NumberFormatException e) { return a.compareTo(b); }
        });
        return keys;
    }

    private String fmtBytes(int n) {
        if (n <= 0) return "—";
        if (n >= 1048576) return String.format(java.util.Locale.US, "%.2f MB", n / 1048576.0);
        if (n >= 1024) return String.format(java.util.Locale.US, "%.1f KB", n / 1024.0);
        return n + " B";
    }

    private String fmtUptime(long s) {
        long d = s / 86400, h = (s % 86400) / 3600, m = (s % 3600) / 60, sec = s % 60;
        return (d > 0 ? d + "d " : "") + String.format(java.util.Locale.US, "%02d:%02d:%02d", h, m, sec);
    }

    private static final class PinCell {
        final View root;
        final TextView lvl;
        boolean high;

        PinCell(View root, TextView lvl) {
            this.root = root;
            this.lvl = lvl;
        }
    }
}
