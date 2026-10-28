package com.lasrecomp.android;

import android.content.Context;
import android.content.res.ColorStateList;
import android.content.res.Configuration;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.view.View;
import android.widget.Button;
import android.widget.TextView;

/** Small platform-widget design system; follows the device's light/dark theme. */
final class RaitchiUi {
    static final class Palette {
        final boolean dark;
        final int background,surface,text,muted,accent,accentSoft;
        Palette(boolean dark) {
            this.dark=dark;
            background=Color.parseColor(dark?"#171316":"#FFF8FA");
            surface=Color.parseColor(dark?"#231D22":"#FFFFFF");
            text=Color.parseColor(dark?"#F8EDF2":"#2C1922");
            muted=Color.parseColor(dark?"#C4ADB9":"#7C626E");
            accent=Color.parseColor(dark?"#FF9EB7":"#B52F58");
            accentSoft=Color.parseColor(dark?"#492335":"#FFE0E9");
        }
    }
    static Palette palette(Context c) { return new Palette((c.getResources().getConfiguration().uiMode&Configuration.UI_MODE_NIGHT_MASK)==Configuration.UI_MODE_NIGHT_YES); }
    static int dp(Context c,float value) { return Math.round(value*c.getResources().getDisplayMetrics().density); }
    static GradientDrawable rounded(int color,float radius) { GradientDrawable d=new GradientDrawable();d.setColor(color);d.setCornerRadius(radius);return d; }
    static TextView text(Context c,String value,float size,Palette p) { TextView t=new TextView(c);t.setText(value);t.setTextSize(size);t.setTextColor(p.text);t.setFontFeatureSettings("kern");return t; }
    static Button button(Context c,String label,boolean primary,Palette p,View.OnClickListener action) {
        Button b=new Button(c);b.setText(label);b.setAllCaps(false);b.setTextSize(15);b.setTypeface(Typeface.create("sans-serif-medium",Typeface.NORMAL));
        b.setTextColor(new ColorStateList(new int[][]{new int[]{-android.R.attr.state_enabled},new int[]{}},new int[]{p.muted,primary?(p.dark?Color.parseColor("#431326"):Color.WHITE):p.accent}));
        b.setBackgroundTintList(ColorStateList.valueOf(primary?p.accent:p.accentSoft));b.setMinHeight(dp(c,48));b.setPadding(dp(c,18),dp(c,6),dp(c,18),dp(c,6));b.setOnClickListener(action);return b;
    }
    private RaitchiUi() {}
}
