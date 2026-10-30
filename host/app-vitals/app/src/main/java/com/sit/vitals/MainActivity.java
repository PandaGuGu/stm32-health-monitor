package com.sit.vitals;

import android.app.Activity;
import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.drawable.GradientDrawable;
import android.os.Build;
import android.os.Bundle;
import android.os.VibrationEffect;
import android.os.Vibrator;
import android.text.InputType;
import android.view.Gravity;
import android.view.View;
import android.view.WindowManager;
import android.view.inputmethod.EditorInfo;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.util.Locale;

/**
 * 健康监测上位机（ESP32 直连）。
 *
 * 数据来源：GET /api/status 的 vitals 段。
 * 显示原则：只信 ok==true 的读数；ok==false 一律显示「无读数」而不是 0，
 * 因为 STM32 门控失败时字段里残留的是上一次的值，直接画出来等于撒谎。
 */
public class MainActivity extends Activity implements VitalsClient.Listener {

    // ---- 配色 ----
    private static final int C_BG     = 0xFF0A1018;
    private static final int C_CARD   = 0xFF141B28;
    private static final int C_TXT    = 0xFFE9EFF7;
    private static final int C_SUB    = 0xFF7C8CA6;
    private static final int C_HR     = 0xFFFF5C7A;
    private static final int C_SPO2   = 0xFF4DA3FF;
    private static final int C_TEMP   = 0xFFFFB74D;
    private static final int C_OK     = 0xFF35D08E;
    private static final int C_WARN   = 0xFFFF4D4D;
    private static final int C_OFF    = 0xFF3A4557;

    private static final long POLL_MS = 1000L;

    private TextView tvStatus;
    private EditText etAddr;
    private Button btnConnect;
    private MetricCard cardHr, cardSpo2, cardTemp, cardFall;
    private TrendView trend;
    private TextView tvFooter;

    private VitalsClient client;
    private SharedPreferences prefs;

    private int failStreak = 0;
    private boolean lastFall = false;

    // ==================================================================
    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

        prefs = getSharedPreferences("vitals", MODE_PRIVATE);
        client = new VitalsClient();

        setContentView(buildUi());

        String saved = prefs.getString("addr", "http://192.168.4.1");
        etAddr.setText(saved);
        client.setBaseUrl(saved);
    }

    @Override
    protected void onResume() {
        super.onResume();
        client.setBaseUrl(etAddr.getText().toString());
        client.start(POLL_MS, this);
        setStatus(false, "连接中…");
    }

    @Override
    protected void onPause() {
        client.stop();
        super.onPause();
    }

    // ==================================================================
    // UI 构建
    // ==================================================================
    private View buildUi() {
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(C_BG);
        int p = dp(16);
        root.setPadding(p, dp(16), p, dp(16));
        root.setFitsSystemWindows(true);

        // --- 标题行 ---
        LinearLayout titleRow = new LinearLayout(this);
        titleRow.setOrientation(LinearLayout.HORIZONTAL);
        titleRow.setGravity(Gravity.CENTER_VERTICAL);

        TextView title = new TextView(this);
        title.setText("健康监测");
        title.setTextColor(C_TXT);
        title.setTextSize(24);
        title.getPaint().setFakeBoldText(true);
        titleRow.addView(title, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT));

        tvStatus = new TextView(this);
        tvStatus.setText("未连接");
        tvStatus.setTextColor(C_SUB);
        tvStatus.setTextSize(13);
        tvStatus.setSingleLine(true);
        tvStatus.setEllipsize(android.text.TextUtils.TruncateAt.END);
        tvStatus.setGravity(Gravity.END);
        LinearLayout.LayoutParams slp = new LinearLayout.LayoutParams(0,
                LinearLayout.LayoutParams.WRAP_CONTENT, 1f);
        slp.leftMargin = dp(12);
        titleRow.addView(tvStatus, slp);
        root.addView(titleRow);

        // --- 地址行 ---
        LinearLayout addrRow = new LinearLayout(this);
        addrRow.setOrientation(LinearLayout.HORIZONTAL);
        addrRow.setGravity(Gravity.CENTER_VERTICAL);
        LinearLayout.LayoutParams arp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        arp.topMargin = dp(14);

        etAddr = new EditText(this);
        etAddr.setHint("http://192.168.4.1");
        etAddr.setTextColor(C_TXT);
        etAddr.setHintTextColor(C_OFF);
        etAddr.setTextSize(14);
        etAddr.setSingleLine(true);
        etAddr.setInputType(InputType.TYPE_TEXT_VARIATION_URI);
        etAddr.setImeOptions(EditorInfo.IME_ACTION_DONE);
        etAddr.setBackground(roundRect(0xFF1B2434, 10, 0, 0));
        etAddr.setPadding(dp(12), dp(8), dp(12), dp(8));
        etAddr.setOnEditorActionListener((v, actionId, ev) -> {
            reconnect();
            return true;
        });
        addrRow.addView(etAddr, new LinearLayout.LayoutParams(0,
                LinearLayout.LayoutParams.WRAP_CONTENT, 1f));

        btnConnect = new Button(this);
        btnConnect.setText("连接");
        btnConnect.setAllCaps(false);
        btnConnect.setTextSize(14);
        btnConnect.setTextColor(0xFF06121F);
        btnConnect.setBackground(roundRect(C_OK, 10, 0, 0));
        btnConnect.setOnClickListener(v -> reconnect());
        LinearLayout.LayoutParams blp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        blp.leftMargin = dp(8);
        addrRow.addView(btnConnect, blp);

        root.addView(addrRow, arp);

        // --- 指标卡片 2x2 ---
        LinearLayout row1 = new LinearLayout(this);
        row1.setOrientation(LinearLayout.HORIZONTAL);
        LinearLayout.LayoutParams rp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        rp.topMargin = dp(16);

        cardHr = new MetricCard(this, "心率", "BPM", C_HR);
        cardSpo2 = new MetricCard(this, "血氧", "%", C_SPO2);
        row1.addView(cardHr, weightParams(1f, 0));
        row1.addView(cardSpo2, weightParams(1f, dp(12)));
        root.addView(row1, rp);

        LinearLayout row2 = new LinearLayout(this);
        row2.setOrientation(LinearLayout.HORIZONTAL);
        LinearLayout.LayoutParams rp2 = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        rp2.topMargin = dp(12);

        cardTemp = new MetricCard(this, "体温", "℃", C_TEMP);
        cardFall = new MetricCard(this, "是否跌倒", "", C_OK);
        row2.addView(cardTemp, weightParams(1f, 0));
        row2.addView(cardFall, weightParams(1f, dp(12)));
        root.addView(row2, rp2);

        // --- 趋势 ---
        LinearLayout trendBox = new LinearLayout(this);
        trendBox.setOrientation(LinearLayout.VERTICAL);
        trendBox.setBackground(roundRect(C_CARD, 16, 0, 0));
        trendBox.setPadding(dp(12), dp(12), dp(12), dp(8));
        LinearLayout.LayoutParams tlp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f);
        tlp.topMargin = dp(12);

        TextView tTitle = new TextView(this);
        tTitle.setText("心率趋势");
        tTitle.setTextColor(C_SUB);
        tTitle.setTextSize(13);
        trendBox.addView(tTitle);

        trend = new TrendView(this);
        trendBox.addView(trend, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f));

        root.addView(trendBox, tlp);

        // --- 页脚状态 ---
        tvFooter = new TextView(this);
        tvFooter.setTextColor(C_OFF);
        tvFooter.setTextSize(11);
        tvFooter.setText("等待连接");
        LinearLayout.LayoutParams flp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        flp.topMargin = dp(10);
        root.addView(tvFooter, flp);

        return root;
    }

    private void reconnect() {
        String a = etAddr.getText().toString().trim();
        if (a.isEmpty()) a = "http://192.168.4.1";
        prefs.edit().putString("addr", a).apply();
        client.stop();
        client.setBaseUrl(a);
        failStreak = 0;
        trend.clear();
        setStatus(false, "连接中…");
        client.start(POLL_MS, this);
    }

    // ==================================================================
    // 数据回调
    // ==================================================================
    @Override
    public void onData(VitalsClient.Snapshot s) {
        failStreak = 0;
        setStatus(true, "已连接 · " + (s.src.isEmpty() ? "uart" : s.src));

        if (s.ok) {
            int hrColor = (s.bpm < 60 || s.bpm > 100) ? C_WARN : C_HR;
            cardHr.setValue(String.valueOf(s.bpm), hrColor, hrNote(s.bpm));

            int spColor = s.spo2 < 95 ? C_WARN : C_SPO2;
            cardSpo2.setValue(String.valueOf(s.spo2), spColor, s.spo2 < 95 ? "偏低" : "正常");

            cardTemp.setValue(fmt1(s.temp), C_TEMP, tempNote(s.temp));

            if (s.fall == 1) {
                cardFall.setValue("是", C_WARN, "检测到跌倒");
                if (!lastFall) buzz();
                lastFall = true;
            } else {
                cardFall.setValue("否", C_OK, "正常");
                lastFall = false;
            }
            trend.push(s.bpm, true);
        } else {
            // 无读数：绝不能把残留值当实时数据画出来。
            // 体温和跌倒来自独立传感器，门控失败时依然可信。
            cardHr.setNoData();
            cardSpo2.setNoData();
            if (s.temp > 0) {
                cardTemp.setValue(fmt1(s.temp), C_TEMP, tempNote(s.temp));
            } else {
                cardTemp.setNoData();
            }
            if (s.fall == 1) {
                cardFall.setValue("是", C_WARN, "检测到跌倒");
                if (!lastFall) buzz();
                lastFall = true;
            } else {
                cardFall.setValue("否", C_OK, "正常");
                lastFall = false;
            }
            trend.push(0, false);
        }

        String age = s.ageMs < 0 ? "从未收到"
                : String.format(Locale.US, "%.1fs 前", s.ageMs / 1000.0);
        tvFooter.setText(String.format(Locale.US,
                "数据 %s · 有效帧 %d · 坏帧 %d · 无读数帧 %d · 运行 %ds · %s %s",
                age, s.frames, s.bad, s.invalid, s.upSec, s.model, s.fw));
    }

    @Override
    public void onError(String msg) {
        failStreak++;
        int bar = msg.indexOf('|');
        String text = bar > 0 ? msg.substring(bar + 1) : msg;
        if (failStreak >= 2) {
            setStatus(false, "断开");
            cardHr.setNoData();
            cardSpo2.setNoData();
            cardFall.setValue("—", C_OFF, "—");
            trend.push(0, false);
            tvFooter.setText("连不上 " + client.getBaseUrl() + "（" + text + "）");
        } else {
            setStatus(false, "重试中");
        }
    }

    private void setStatus(boolean live, String text) {
        tvStatus.setText(text);
        tvStatus.setTextColor(live ? C_OK : (failStreak >= 2 ? C_WARN : C_SUB));
    }

    private void buzz() {
        try {
            Vibrator v = (Vibrator) getSystemService(Context.VIBRATOR_SERVICE);
            if (v == null || !v.hasVibrator()) return;
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                v.vibrate(VibrationEffect.createOneShot(400, VibrationEffect.DEFAULT_AMPLITUDE));
            } else {
                v.vibrate(400);
            }
        } catch (Exception ignored) {
        }
    }

    private static String hrNote(int bpm) {
        if (bpm < 60) return "偏慢";
        if (bpm > 100) return "偏快";
        return "正常";
    }

    private static String tempNote(double t) {
        if (t <= 0) return "无读数";
        if (t < 36.0) return "偏低";
        if (t > 37.3) return "偏高";
        return "正常";
    }

    private static String fmt1(double v) {
        return String.format(Locale.US, "%.1f", v);
    }

    // ==================================================================
    // 小工具
    // ==================================================================
    private int dp(float v) {
        return Math.round(v * getResources().getDisplayMetrics().density);
    }

    private static LinearLayout.LayoutParams weightParams(float w, int leftMargin) {
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(0,
                LinearLayout.LayoutParams.WRAP_CONTENT, w);
        lp.leftMargin = leftMargin;
        return lp;
    }

    private GradientDrawable roundRect(int fill, int radiusDp, int strokeW, int strokeColor) {
        GradientDrawable d = new GradientDrawable();
        d.setColor(fill);
        d.setCornerRadius(dp(radiusDp));
        if (strokeW > 0) d.setStroke(strokeW, strokeColor);
        return d;
    }

    // ==================================================================
    /** 单个指标卡片：标题 / 大数字 / 状态注脚。 */
    private class MetricCard extends LinearLayout {
        private final TextView tvValue;
        private final TextView tvNote;
        private final int accent;

        MetricCard(Context c, String name, String unit, int accent) {
            super(c);
            this.accent = accent;
            setOrientation(VERTICAL);
            setPadding(dp(14), dp(12), dp(14), dp(12));
            setBackground(roundRect(C_CARD, 16, 0, 0));

            TextView t = new TextView(c);
            t.setText(name + (unit.isEmpty() ? "" : " · " + unit));
            t.setTextColor(C_SUB);
            t.setTextSize(13);
            addView(t);

            tvValue = new TextView(c);
            tvValue.setText("—");
            tvValue.setTextColor(C_OFF);
            tvValue.setTextSize(38);
            tvValue.getPaint().setFakeBoldText(true);
            LinearLayout.LayoutParams vp = new LinearLayout.LayoutParams(
                    LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT);
            vp.topMargin = dp(2);
            addView(tvValue, vp);

            tvNote = new TextView(c);
            tvNote.setText("—");
            tvNote.setTextColor(C_OFF);
            tvNote.setTextSize(12);
            addView(tvNote);
        }

        void setValue(String v, int color, String note) {
            tvValue.setText(v);
            tvValue.setTextColor(color);
            tvNote.setText(note);
            tvNote.setTextColor(color);
        }

        void setNoData() {
            tvValue.setText("—");
            tvValue.setTextColor(C_OFF);
            tvNote.setText("无读数");
            tvNote.setTextColor(C_OFF);
        }
    }
}
