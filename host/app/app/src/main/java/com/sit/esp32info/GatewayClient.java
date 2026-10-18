package com.sit.esp32info;

import org.json.JSONObject;

import java.io.BufferedReader;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;

/** 网关 REST 客户端：只有 GET / POST 两个动作，够用且不引依赖。 */
final class GatewayClient {

    volatile String base = "http://10.0.2.2:8080";

    private String url(String path) {
        String b = base.trim();
        if (!b.startsWith("http")) b = "http://" + b;
        if (b.endsWith("/")) b = b.substring(0, b.length() - 1);
        return b + path;
    }

    JSONObject get(String path, int timeoutMs) throws Exception {
        HttpURLConnection c = (HttpURLConnection) new URL(url(path)).openConnection();
        c.setConnectTimeout(timeoutMs);
        c.setReadTimeout(timeoutMs);
        c.setRequestMethod("GET");
        return read(c);
    }

    JSONObject post(String path, JSONObject body, int timeoutMs) throws Exception {
        HttpURLConnection c = (HttpURLConnection) new URL(url(path)).openConnection();
        c.setConnectTimeout(timeoutMs);
        c.setReadTimeout(timeoutMs);
        c.setRequestMethod("POST");
        c.setDoOutput(true);
        c.setRequestProperty("Content-Type", "application/json; charset=utf-8");
        byte[] out = body.toString().getBytes(StandardCharsets.UTF_8);
        c.setFixedLengthStreamingMode(out.length);
        try (OutputStream os = c.getOutputStream()) {
            os.write(out);
        }
        return read(c);
    }

    private JSONObject read(HttpURLConnection c) throws Exception {
        int code = c.getResponseCode();
        InputStream is = (code >= 200 && code < 300) ? c.getInputStream() : c.getErrorStream();
        StringBuilder sb = new StringBuilder();
        if (is != null) {
            try (BufferedReader br = new BufferedReader(new InputStreamReader(is, StandardCharsets.UTF_8))) {
                String line;
                while ((line = br.readLine()) != null) sb.append(line);
            }
        }
        c.disconnect();
        if (sb.length() == 0) throw new IllegalStateException("HTTP " + code + " 空响应");
        return new JSONObject(sb.toString());
    }
}
