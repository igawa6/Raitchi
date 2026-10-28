package com.lasrecomp.android;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.content.res.ColorStateList;
import android.os.Bundle;
import android.view.View;
import android.widget.*;

/** Global defaults and explicit title overrides; applied at the next game launch. */
public final class SettingsActivity extends Activity {
    public static final String EXTRA_TITLE="title";
    private static final int IMPORT_DRIVER=301;
    private SettingsStore store;
    private String title;
    private RaitchiUi.Palette colors;
    private LinearLayout body;
    private final android.os.Handler refreshHandler=new android.os.Handler(android.os.Looper.getMainLooper());
    private long driverRevision=-1;
    private final Runnable driverRefresh=new Runnable(){public void run(){
        if(driverRevision!=RaitchiUiDriverImport.revision())render();
        refreshHandler.postDelayed(this,500);
    }};
    @Override protected void onResume(){super.onResume();render();refreshHandler.removeCallbacks(driverRefresh);refreshHandler.post(driverRefresh);}
    @Override protected void onPause(){refreshHandler.removeCallbacks(driverRefresh);super.onPause();}

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        String requested=getIntent().getStringExtra(EXTRA_TITLE);
        title=requested==null?null:SharedFiles.checked(requested);
        store=new SettingsStore(this);render();
    }
    private int dp(float n) { return RaitchiUi.dp(this,n); }
    private void render() {
        colors=RaitchiUi.palette(this);driverRevision=RaitchiUiDriverImport.revision();
        boolean driverBusy=RaitchiUiDriverImport.isRunning();
        getWindow().setStatusBarColor(colors.background);getWindow().setNavigationBarColor(colors.background);
        ScrollView scroll=new ScrollView(this);scroll.setFillViewport(true);scroll.setBackgroundColor(colors.background);
        body=new LinearLayout(this);body.setOrientation(LinearLayout.VERTICAL);body.setPadding(dp(28),dp(20),dp(28),dp(32));
        scroll.addView(body,new ScrollView.LayoutParams(-1,-2));setContentView(scroll);
        body.addView(RaitchiUi.button(this,"‹  Library",false,colors,v->finish()));
        text(title==null?"Settings":SharedFiles.label(title)+" settings",28,colors.text);
        text(title==null?"Defaults for all games. Each game can override these settings.":"Inherited settings follow your global defaults. Changes apply on the next launch.",14,colors.muted);
        if(title==null)text("Changes apply on the next launch. Raitchi follows your system’s light or dark appearance.",13,colors.muted);
        String category="";
        for(SettingsStore.Option option:SettingsStore.OPTIONS) {
            if(option.lasOnly&&title!=null&&!"las".equals(title))continue;
            if(TitlePolicy.nativeOnly(title)&&option.key.equals("cpu_backend")){heading("General");text("Continuous NCE · original ARM64 execution",14,colors.text);continue;}
            if(!category.equals(option.category)) {category=option.category;heading(category);}
            String suffix=title==null?"":store.hasOverride(title,option.key)?" · This game":" · Global";
            Button row=RaitchiUi.button(this,option.label+"\n"+option.describe(store.getString(title,option.key))+suffix,false,colors,v->choose(option));
            row.setGravity(android.view.Gravity.START|android.view.Gravity.CENTER_VERTICAL);
            row.setPadding(dp(16),dp(12),dp(16),dp(12));body.addView(row,new LinearLayout.LayoutParams(-1,-2));
            if(option.key.equals("cpu_backend"))text("Continuous NCE runs original ARM64 code and uses a separate save session. Compatibility testing is ongoing; AOT is available as an alternative.",12,colors.muted);
            if(title==null&&option.lasOnly)text("Applies to Link’s Awakening.",12,colors.muted);
        }
        heading("Drivers");
        if(!RaitchiUiDriverImport.status().isEmpty())text(RaitchiUiDriverImport.status(),13,colors.muted);
        text("Graphics driver selection is shared by all games.",13,colors.muted);
        GpuDrivers.Driver driver=GpuDrivers.find(this);
        text(driver==null?"No custom driver installed":driver.name+" · "+driver.version,16,colors.text);
        Switch enabled=new Switch(this);enabled.setText("Use installed graphics driver");enabled.setTextColor(colors.text);
        enabled.setChecked(GpuDrivers.selected(this));enabled.setEnabled(driver!=null&&!driverBusy);
        enabled.setThumbTintList(ColorStateList.valueOf(colors.accent));
        enabled.setOnCheckedChangeListener((button,on)->{GpuDrivers.select(this,on);render();});body.addView(enabled);
        Button install=RaitchiUi.button(this,driverBusy?"Installing driver…":"Install driver ZIP",false,colors,v->{
            Intent pick=new Intent(Intent.ACTION_OPEN_DOCUMENT).setType("*/*").addCategory(Intent.CATEGORY_OPENABLE);
            startActivityForResult(pick,IMPORT_DRIVER);
        });install.setEnabled(!driverBusy);body.addView(install);
        text("Link’s Awakening requires an enabled custom driver for readable menu text on Thor. Installing another driver replaces the current selection.",12,colors.muted);
        heading("System policy");
        text("Let’s Go Eevee uses handheld controller mode. Link’s Awakening uses a full controller. Required title policies are applied automatically.",13,colors.muted);
        body.addView(RaitchiUi.button(this,title==null?"Restore default settings":"Use global settings for this game",false,colors,v->new AlertDialog.Builder(this)
            .setTitle("Reset settings?").setMessage("Your games, saves and graphics driver will be kept.")
            .setNegativeButton("Cancel",null).setPositiveButton("Reset",(d,w)->{store.reset(title);render();}).show()));
    }
    private void text(String value,float size,int color) {
        TextView view=RaitchiUi.text(this,value,size,colors);view.setTextColor(color);view.setPadding(0,dp(6),0,dp(8));body.addView(view);
    }
    private void heading(String label) { text(label,20,colors.accent); }
    private void choose(SettingsStore.Option option) {
        int offset=title==null?0:1;
        String[] choices=new String[option.labels.length+offset];
        if(offset==1)choices[0]="Use global · "+option.describe(store.getString(null,option.key));
        System.arraycopy(option.labels,0,choices,offset,option.labels.length);
        int selected=offset==1&&!store.hasOverride(title,option.key)?0:-1;
        if(selected<0)for(int i=0;i<option.values.length;i++)if(option.values[i].equals(store.getString(title,option.key)))selected=i+offset;
        new AlertDialog.Builder(this).setTitle(option.label).setSingleChoiceItems(choices,selected,(dialog,index)->{
            dialog.dismiss();
            if(offset==1&&index==0) {store.clearOverride(title,option.key);render();return;}
            String value=option.values[index-offset];
            store.setString(title,option.key,value);render();
        }).setNegativeButton("Cancel",null).show();
    }
    @Override protected void onActivityResult(int request,int result,Intent data) {
        super.onActivityResult(request,result,data);
        if(request!=IMPORT_DRIVER||result!=RESULT_OK||data==null||data.getData()==null)return;
        RaitchiUiDriverImport.start(this,data.getData());
        render();
    }
}
