package com.lasrecomp.android;

import android.content.Context;
import android.content.SharedPreferences;
import java.util.LinkedHashMap;
import java.util.Map;

/** Validated launch settings. An absent title key inherits the global value. */
public final class SettingsStore {
    public static final class Option {
        public final String key, category, label, initial;
        public final String[] values, labels;
        public final boolean lasOnly;
        Option(String key,String category,String label,String initial,String[] values,String[] labels,boolean lasOnly) {
            this.key=key;this.category=category;this.label=label;this.initial=initial;this.values=values;this.labels=labels;this.lasOnly=lasOnly;
        }
        public String describe(String value) {
            for(int i=0;i<values.length;i++) if(values[i].equals(value)) return labels[i];
            return initial;
        }
        boolean accepts(String value) { for(String v:values) if(v.equals(value))return true; return false; }
    }
    private static Option choice(String key,String category,String label,String initial,String[] values,String[] labels) {
        return new Option(key,category,label,initial,values,labels,false);
    }
    private static Option toggle(String key,String category,String label,boolean initial) {
        return choice(key,category,label,initial?"1":"0",new String[]{"1","0"},new String[]{"On","Off"});
    }
    public static final Option[] OPTIONS={
        choice("cpu_backend","General","CPU execution","continuous-nce",new String[]{"continuous-nce","aot"},new String[]{"Continuous NCE (default)","Recompiled (AOT)"}),
        toggle("show_fps","General","Show frame rate",true),
        choice("language","System","Game language","auto",new String[]{"auto","ja","en-US","fr","de","it","es","zh-Hans","ko","nl","ru","zh-Hant"},new String[]{"Follow system","Japanese","English","French","German","Italian","Spanish","Chinese (Simplified)","Korean","Dutch","Russian","Chinese (Traditional)"}),
        choice("render_scale","Graphics","Render resolution","0.6666666666666666",new String[]{"0.5","0.6666666666666666","1","0"},new String[]{"0.5× native","0.667× native (recommended)","1× native","Dynamic"}),
        choice("upscaler","Graphics","Upscaling","bilinear",new String[]{"bilinear","sgsr"},new String[]{"Bilinear","Snapdragon Game Super Resolution"}),
        choice("antialias","Graphics","Anti-aliasing","off",new String[]{"off","smaa"},new String[]{"Off","SMAA"}),
        new Option("frame_rate","Graphics","Game frame rate","2",new String[]{"2","0","1"},new String[]{"30 fps (recommended)","Automatic","60 fps (may slow gameplay)"},true),
        new Option("aspect","Graphics","Aspect ratio","original",new String[]{"original","unlocked"},new String[]{"Original 16:9","Fit screen"},true),
        new Option("blur_removal","Graphics","Remove tilt-shift blur","0",new String[]{"1","0"},new String[]{"On","Off"},true),
        choice("volume","Audio","Master volume","1.00",new String[]{"0.00","0.25","0.50","0.75","1.00"},new String[]{"Muted","25%","50%","75%","100%"}),
        toggle("reverb","Audio","Reverb",true),
        toggle("movie_audio","Audio","Movie audio",true),
        choice("touch_controls","Controls","Touch controls","auto",new String[]{"auto","always","off"},new String[]{"When no controller is connected","Always","Off"}),
        toggle("rumble","Controls","Controller vibration",true),
        choice("rumble_strength","Controls","Vibration strength","100",new String[]{"25","50","75","100"},new String[]{"25%","50%","75%","100%"})
    };
    private final SharedPreferences prefs;
    public SettingsStore(Context context) { prefs=context.getSharedPreferences("raitchi_settings",Context.MODE_PRIVATE); }
    private static String scope(String title) {
        if(title==null)return "global/";
        TitlePolicy.get(title);
        return "game/"+title+"/";
    }
    public static Option option(String key) {
        for(Option o:OPTIONS)if(o.key.equals(key))return o;
        throw new IllegalArgumentException("Unknown setting: "+key);
    }
    public String getString(String title,String key) {
        if(TitlePolicy.nativeOnly(title)&&"cpu_backend".equals(key))return "continuous-nce";
        Option o=option(key);String global=prefs.getString(scope(null)+key,o.initial);
        if(!o.accepts(global))global=o.initial;
        String result=prefs.getString(scope(title)+key,global);
        return o.accepts(result)?result:global;
    }
    public boolean getBoolean(String title,String key) { return "1".equals(getString(title,key)); }
    public boolean hasOverride(String title,String key) { option(key);return title!=null&&prefs.contains(scope(title)+key); }
    public void setString(String title,String key,String value) {
        if(TitlePolicy.nativeOnly(title)&&"cpu_backend".equals(key)&&!"continuous-nce".equals(value))throw new IllegalArgumentException("This title requires continuous NCE");
        if(!option(key).accepts(value))throw new IllegalArgumentException("Invalid setting value");
        if(!prefs.edit().putString(scope(title)+key,value).commit())throw new IllegalStateException("Cannot save settings");
    }
    public void setBoolean(String title,String key,boolean value) { setString(title,key,value?"1":"0"); }
    public void clearOverride(String title,String key) {
        if(title==null)throw new IllegalArgumentException("Global settings do not inherit");
        option(key);if(!prefs.edit().remove(scope(title)+key).commit())throw new IllegalStateException("Cannot save settings");
    }
    public void reset(String title) {
        SharedPreferences.Editor edit=prefs.edit();for(Option o:OPTIONS)edit.remove(scope(title)+o.key);
        if(!edit.commit())throw new IllegalStateException("Cannot reset settings");
    }
    public Map<String,String> ini(String title) {
        scope(title);Map<String,String> values=new LinkedHashMap<>();
        for(Option o:OPTIONS)if(!o.key.equals("cpu_backend")&&!o.key.equals("render_scale")&&(!o.lasOnly||"las".equals(title)))values.put(o.key,getString(title,o.key));
        values.put("resolution","0".equals(getString(title,"render_scale"))?"dynamic":"1");
        values.put("fullscreen","1");values.put("frame_gen","0");values.put("power_mode","0");
        return values;
    }
    public Map<String,String> environment(String title) {
        Map<String,String> values=new LinkedHashMap<>();
        values.put("LARECOMP_CPU_BACKEND",getString(title,"cpu_backend"));
        values.put("LARECOMP_SCALE",getString(title,"render_scale"));
        values.put("LC_ALL",java.util.Locale.getDefault().toLanguageTag().replace('-','_'));
        return values;
    }
    public String sessionSuffix(String title) {
        return "continuous-nce".equals(getString(title,"cpu_backend"))?"-nce":"";
    }
    /** Replace owned keys while retaining unknown settings and comments verbatim. */
    public static String merge(String original,Map<String,String> values) {
        return merge(original,values," = ");
    }
    public static String mergeEnvironment(String original,Map<String,String> values) {
        return merge(original,values,"=");
    }
    private static String merge(String original,Map<String,String> values,String separator) {
        StringBuilder out=new StringBuilder();
        for(String line:original.split("\\r?\\n",-1)) {
            int equals=line.indexOf('=');String key=equals<0?"":line.substring(0,equals).trim();
            if(!line.isEmpty()&&!values.containsKey(key))out.append(line).append('\n');
        }
        for(Map.Entry<String,String> item:values.entrySet())out.append(item.getKey()).append(separator).append(item.getValue()).append('\n');
        return out.toString();
    }
}
