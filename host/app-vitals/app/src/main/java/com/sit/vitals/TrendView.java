package com.sit.vitals;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.util.AttributeSet;
import android.view.View;

import java.util.ArrayList;

/**
 * 心率趋势曲线。
 *
 * 只画「有效」采样点，无读数的时段直接断开——这样一眼能看出门控什么时候丢了信号，
 * 而不是被一条假的下滑线糊过去。
 */
public class TrendView extends View {

    private static final int CAPACITY = 90;

    private static class Pt {
        final float value;
        final boolean valid;
        Pt(float v, boolean ok) { value = v; valid = ok; }
    }

    private final ArrayList<Pt> pts = new ArrayList<>();
    private final Paint grid = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint line = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint dot = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint label = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Path path = new Path();
    private final Path area = new Path();
    private final RectF box = new RectF();

    private int lo = 50, hi = 130;   // 纵轴范围（bpm），随数据自适应

    public TrendView(Context c) { this(c, null); }

    public TrendView(Context c, AttributeSet a) {
        super(c, a);
        setLayerType(LAYER_TYPE_SOFTWARE, null);

        grid.setColor(0x1AFFFFFF);
        grid.setStrokeWidth(1f);

        line.setColor(0xFFFF5C7A);
        line.setStrokeWidth(2.4f);
        line.setStyle(Paint.Style.STROKE);
        line.setStrokeJoin(Paint.Join.ROUND);
        line.setStrokeCap(Paint.Cap.ROUND);

        fill.setColor(0x33FF5C7A);
        fill.setStyle(Paint.Style.FILL);

        dot.setColor(0xFFFF5C7A);

        label.setColor(0x8AFFFFFF);
        label.setTextSize(24f);
    }

    public void push(int bpm, boolean valid) {
        if (pts.size() >= CAPACITY) pts.remove(0);
        pts.add(new Pt(bpm, valid));
        if (valid && bpm > 0) {
            // 纵轴自适应，留 15 的边距，避免贴边
            if (bpm < lo) lo = Math.max(30, bpm - 15);
            if (bpm > hi) hi = Math.min(220, bpm + 15);
        }
        invalidate();
    }

    public void clear() {
        pts.clear();
        lo = 50;
        hi = 130;
        invalidate();
    }

    @Override
    protected void onDraw(Canvas cv) {
        int w = getWidth();
        int h = getHeight();
        if (w <= 0 || h <= 0) return;

        float padL = 44f, padR = 12f, padT = 14f, padB = 26f;
        box.set(padL, padT, w - padR, h - padB);

        // 横向网格 + 刻度
        int rows = 4;
        for (int i = 0; i <= rows; i++) {
            float y = box.top + box.height() * i / rows;
            cv.drawLine(box.left, y, box.right, y, grid);
            int val = Math.round(hi - (hi - lo) * i / (float) rows);
            cv.drawText(String.valueOf(val), 8f, y + 8f, label);
        }

        if (pts.size() < 2) {
            label.setTextSize(26f);
            cv.drawText("等待数据…", box.centerX() - 60f, box.centerY(), label);
            label.setTextSize(24f);
            return;
        }

        float dx = box.width() / (CAPACITY - 1);
        float offset = box.width() - (pts.size() - 1) * dx;
        float span = Math.max(1f, hi - lo);

        path.reset();
        area.reset();
        boolean started = false;
        float lastX = 0, lastY = 0;
        boolean lastValid = false;

        for (int i = 0; i < pts.size(); i++) {
            Pt p = pts.get(i);
            float x = offset + i * dx;
            if (!p.valid || p.value <= 0) {
                started = false;      // 断开，下一段重新起笔
                lastValid = false;
                continue;
            }
            float v = Math.max(lo, Math.min(hi, p.value));
            float y = box.bottom - (v - lo) / span * box.height();
            if (!started) {
                path.moveTo(x, y);
                started = true;
            } else {
                path.lineTo(x, y);
            }
            lastX = x;
            lastY = y;
            lastValid = true;
        }
        cv.drawPath(path, line);

        // 末端高亮点
        if (lastValid) {
            cv.drawCircle(lastX, lastY, 5.5f, dot);
        }
    }
}
