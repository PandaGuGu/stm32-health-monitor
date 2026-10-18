package com.sit.esp32info;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.LinearGradient;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.Shader;
import android.util.AttributeSet;
import android.view.View;

/** 迷你趋势线：只用 Canvas 画折线 + 渐变填充，不引图表库。 */
public class SparkView extends View {

    private float[] data = new float[0];
    private int color = 0xFF58A6FF;
    private final Paint line = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint base = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint dot = new Paint(Paint.ANTI_ALIAS_FLAG);

    public SparkView(Context c) { this(c, null); }

    public SparkView(Context c, AttributeSet a) {
        super(c, a);
        line.setStyle(Paint.Style.STROKE);
        line.setStrokeWidth(getResources().getDisplayMetrics().density * 1.5f);
        line.setStrokeCap(Paint.Cap.ROUND);
        line.setStrokeJoin(Paint.Join.ROUND);
        fill.setStyle(Paint.Style.FILL);
        base.setColor(0x14FFFFFF);
        base.setStrokeWidth(1f);
        dot.setStyle(Paint.Style.FILL);
    }

    public void setData(float[] d, int c) {
        this.data = d == null ? new float[0] : d;
        this.color = c;
        invalidate();
    }

    @Override
    protected void onDraw(Canvas cv) {
        super.onDraw(cv);
        int w = getWidth(), h = getHeight();
        if (w <= 0 || h <= 0) return;

        cv.drawLine(0, h - 0.5f, w, h - 0.5f, base);
        if (data.length < 2) return;

        float min = data[0], max = data[0];
        for (float v : data) { if (v < min) min = v; if (v > max) max = v; }
        if (max - min < 0.001f) { max += 1; min -= 1; }
        float pad = (max - min) * 0.15f;
        min -= pad; max += pad;

        Path p = new Path();
        Path area = new Path();
        for (int i = 0; i < data.length; i++) {
            float x = (i / (float) (data.length - 1)) * (w - 2) + 1;
            float y = h - 3 - ((data[i] - min) / (max - min)) * (h - 8);
            if (i == 0) { p.moveTo(x, y); area.moveTo(x, y); }
            else { p.lineTo(x, y); area.lineTo(x, y); }
        }
        area.lineTo(w - 1, h);
        area.lineTo(1, h);
        area.close();

        fill.setShader(new LinearGradient(0, 0, 0, h,
                (color & 0x00FFFFFF) | 0x55000000, color & 0x00FFFFFF, Shader.TileMode.CLAMP));
        cv.drawPath(area, fill);

        line.setColor(color);
        cv.drawPath(p, line);

        dot.setColor(color);
        float lx = w - 1;
        float ly = h - 3 - ((data[data.length - 1] - min) / (max - min)) * (h - 8);
        cv.drawCircle(lx, ly, 2.6f * getResources().getDisplayMetrics().density, dot);
    }
}
