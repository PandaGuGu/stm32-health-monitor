package com.sit.esp32info;

import android.content.Context;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.util.LinkedHashMap;
import java.util.Map;

/** 极简 UI 构造工具：卡片 / 键值网格 / 圆角背景，全部代码生成，不依赖任何第三方库。 */
final class Ui {

    static final int BG      = 0xFF0B0E14;
    static final int CARD    = 0xFF151A23;
    static final int CARD_2  = 0xFF0E131B;
    static final int LINE    = 0xFF232B38;
    static final int TEXT    = 0xFFE6EDF3;
    static final int MUTED   = 0xFF8B98A9;
    static final int DIM     = 0xFF5F6B7C;
    static final int ACCENT  = 0xFF58A6FF;
    static final int OK      = 0xFF3FB950;
    static final int WARN    = 0xFFD29922;
    static final int ERR     = 0xFFF85149;
    static final int GREEN   = 0xFF7EE787;
    static final int PURPLE  = 0xFFBC8CFF;

    private Ui() {}

    static int dp(Context c, float v) {
        return Math.round(TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v,
                c.getResources().getDisplayMetrics()));
    }

    static GradientDrawable bg(int color, int strokeColor, float radiusDp, Context c) {
        GradientDrawable d = new GradientDrawable();
        d.setShape(GradientDrawable.RECTANGLE);
        d.setColor(color);
        d.setCornerRadius(dp(c, radiusDp));
        if (strokeColor != 0) d.setStroke(Math.max(1, dp(c, 0.7f)), strokeColor);
        return d;
    }

    static TextView text(Context c, String s, float sp, int color, boolean mono) {
        TextView t = new TextView(c);
        t.setText(s);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        t.setTextColor(color);
        if (mono) t.setTypeface(Typeface.MONOSPACE);
        return t;
    }

    static LinearLayout col(Context c) {
        LinearLayout l = new LinearLayout(c);
        l.setOrientation(LinearLayout.VERTICAL);
        return l;
    }

    static LinearLayout row(Context c) {
        LinearLayout l = new LinearLayout(c);
        l.setOrientation(LinearLayout.HORIZONTAL);
        l.setGravity(Gravity.CENTER_VERTICAL);
        return l;
    }

    /** 一张卡片：标题行 + 内容区 */
    static LinearLayout card(Context c, String title, View body, View right) {
        LinearLayout card = col(c);
        card.setBackground(bg(CARD, LINE, 12, c));
        int p = dp(c, 13);
        card.setPadding(p, p, p, p);

        LinearLayout head = row(c);
        TextView t = text(c, title, 13, 0xFFCBD5E1, false);
        t.setTypeface(Typeface.DEFAULT_BOLD);
        head.addView(t, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        if (right != null) head.addView(right);
        card.addView(head, matchWrap(dp(c, 10)));
        if (body != null) card.addView(body);
        return card;
    }

    static LinearLayout.LayoutParams matchWrap(int bottomMargin) {
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.bottomMargin = bottomMargin;
        return lp;
    }

    static LinearLayout.LayoutParams wrapWrap() {
        return new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
    }

    static LinearLayout.LayoutParams weight1() {
        return new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
    }

    /** 键值网格：自动按列数换行，put() 更新值 */
    static final class KvGrid {
        final LinearLayout root;
        final int cols;
        final Map<String, TextView> values = new LinkedHashMap<>();
        LinearLayout curRow;
        int inRow;

        KvGrid(Context c, int cols) {
            this.cols = cols;
            root = col(c);
        }

        void add(Context c, String key, String label) {
            if (curRow == null || inRow == cols) {
                curRow = row(c);
                curRow.setPadding(0, Ui.dp(c, 4), 0, 0);
                root.addView(curRow);
                inRow = 0;
            }
            LinearLayout cell = col(c);
            TextView k = text(c, label, 10, DIM, false);
            TextView v = text(c, "—", 12.5f, TEXT, true);
            v.setSingleLine(true);
            v.setEllipsize(android.text.TextUtils.TruncateAt.END);
            v.setPadding(0, Ui.dp(c, 1), 0, 0);
            cell.addView(k);
            cell.addView(v);
            LinearLayout.LayoutParams lp = weight1();
            lp.rightMargin = Ui.dp(c, 8);
            curRow.addView(cell, lp);
            values.put(key, v);
            inRow++;
        }

        void put(String key, String value, int color) {
            TextView v = values.get(key);
            if (v == null) return;
            v.setText(value == null ? "—" : value);
            v.setTextColor(color);
        }

        void put(String key, String value) {
            put(key, value, TEXT);
        }
    }
}
