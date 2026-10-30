package com.sit.vitals;

import android.os.Handler;
import android.os.Looper;

import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;

/**
 * 直连 ESP32 的 HTTP 客户端。
 *
 * 只依赖 HttpURLConnection + Android 自带的 org.json，零第三方库。
 * 轮询 GET /api/status（与 PC 网关同构的快照），把 vitals 段解析成 Snapshot。
 *
 * 关于「无读数」的处理见 VitalsSnapshot 里 ok / hasSeen 的区别：设备在门控失败时
 * 仍会发 bpm=0 的帧，固件已把这类帧归入 invalid 且不刷新 age_ms，因此 App 只信
 * ok==true 的数值，绝不把 0 当真实心率显示。
 */
public class VitalsClient {

    /** 一次快照。字段与协议 /api/status 的 vitals 段一一对应。 */
    public static class Snapshot {
        public boolean ok;              // 8 s 内有有效帧
        public int bpm;
        public int spo2;
        public int fall;
        public double temp;
        public String src = "none";
        public long ageMs = -1;         // 距最后有效帧；-1 表示从未收到
        public long frames;
        public long bad;
        public long invalid;
        public long upSec;
        public String fw = "";
        public String model = "";
        public long recvAt = System.currentTimeMillis();
    }

    public interface Listener {
        void onData(Snapshot s);
        void onError(String message);
    }

    private final Handler main = new Handler(Looper.getMainLooper());

    private String baseUrl = "http://192.168.4.1";
    private volatile boolean running = false;
    private volatile Thread worker;

    public void setBaseUrl(String url) {
        String u = url == null ? "" : url.trim();
        while (u.endsWith("/")) u = u.substring(0, u.length() - 1);
        if (!u.startsWith("http://") && !u.startsWith("https://")) u = "http://" + u;
        baseUrl = u;
    }

    public String getBaseUrl() {
        return baseUrl;
    }

    /** 开始按 intervalMs 轮询。重复调用会先停掉旧的。 */
    public void start(final long intervalMs, final Listener listener) {
        stop();
        running = true;
        worker = new Thread(new Runnable() {
            @Override
            public void run() {
                int failStreak = 0;
                while (running) {
                    long t0 = System.currentTimeMillis();
                    try {
                        Snapshot s = fetchOnce();
                        failStreak = 0;
                        final Snapshot fs = s;
                        main.post(new Runnable() {
                            @Override public void run() { listener.onData(fs); }
                        });
                    } catch (final Exception e) {
                        failStreak++;
                        final String msg = describe(e);
                        final int n = failStreak;
                        main.post(new Runnable() {
                            @Override public void run() { listener.onError(n + "|" + msg); }
                        });
                    }
                    long spent = System.currentTimeMillis() - t0;
                    long wait = intervalMs - spent;
                    if (wait < 120) wait = 120;
                    try {
                        Thread.sleep(wait);
                    } catch (InterruptedException ie) {
                        break;
                    }
                }
            }
        }, "vitals-poll");
        worker.setDaemon(true);
        worker.start();
    }

    public void stop() {
        running = false;
        Thread w = worker;
        worker = null;
        if (w != null) w.interrupt();
    }

    /** 同步取一次，供连接测试按钮复用。 */
    public Snapshot fetchOnce() throws Exception {
        String body;
        HttpURLConnection c = null;
        try {
            URL url = new URL(baseUrl + "/api/status");
            c = (HttpURLConnection) url.openConnection();
            c.setConnectTimeout(2500);
            c.setReadTimeout(3000);
            c.setRequestMethod("GET");
            c.setRequestProperty("Accept", "application/json");
            c.setUseCaches(false);
            int code = c.getResponseCode();
            if (code != 200) throw new IllegalStateException("HTTP " + code);
            body = readAll(c.getInputStream());
        } finally {
            if (c != null) c.disconnect();
        }
        return parse(body);
    }

    private static String readAll(InputStream in) throws Exception {
        try {
            ByteArrayOutputStream bos = new ByteArrayOutputStream(4096);
            byte[] buf = new byte[2048];
            int n;
            while ((n = in.read(buf)) > 0) bos.write(buf, 0, n);
            return bos.toString("UTF-8");
        } finally {
            in.close();
        }
    }

    static Snapshot parse(String body) throws Exception {
        // 设备有时会在响应前混入调试文本，从第一个 { 开始截
        int b = body.indexOf('{');
        int e = body.lastIndexOf('}');
        if (b < 0 || e <= b) throw new IllegalStateException("响应不是 JSON");
        JSONObject root = new JSONObject(body.substring(b, e + 1));

        Snapshot s = new Snapshot();
        JSONObject v = root.optJSONObject("vitals");
        if (v != null) {
            s.ok = v.optBoolean("ok", false);
            s.bpm = v.optInt("bpm", 0);
            s.spo2 = v.optInt("spo2", 0);
            s.fall = v.optInt("fall", 0);
            s.temp = v.optDouble("temp", 0);
            s.src = v.optString("src", "none");
            s.ageMs = v.isNull("age_ms") ? -1 : v.optLong("age_ms", -1);
            s.frames = v.optLong("frames", 0);
            s.bad = v.optLong("bad", 0);
            s.invalid = v.optLong("invalid", 0);
        }
        JSONObject rt = root.optJSONObject("runtime");
        if (rt != null) s.upSec = rt.optLong("up", 0);
        JSONObject dev = root.optJSONObject("device");
        if (dev != null) {
            s.fw = dev.optString("fw", "");
            s.model = dev.optString("model", "");
        }
        return s;
    }

    private static String describe(Exception e) {
        String m = e.getMessage();
        if (m == null) m = e.getClass().getSimpleName();
        if (m.contains("timed out") || m.contains("timeout")) return "连接超时";
        if (m.contains("Failed to connect") || m.contains("refused")) return "拒绝连接";
        if (m.contains("Network unreachable") || m.contains("ENETUNREACH")) return "不通网络";
        if (m.contains("Host") || m.contains("UnknownHost")) return "地址解析失败";
        return m;
    }
}
