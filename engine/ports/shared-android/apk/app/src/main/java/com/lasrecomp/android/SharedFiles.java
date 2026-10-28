package com.lasrecomp.android;

import android.content.Context;
import org.json.JSONObject;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.UUID;
import java.util.Map;
import java.util.LinkedHashMap;
import java.util.Arrays;
import java.util.HashSet;
import java.util.Set;

/** Session and build paths belong to this app, never the reference installations. */
final class SharedFiles {
    static final String[] TITLES = TitlePolicy.TITLES;
    static String label(String title) { return TitlePolicy.get(title).label; }
    static String checked(String title) { TitlePolicy.get(title);return title; }
    static File root(Context c) { return c.getFilesDir(); }
    static File game(Context c, String t) { return new File(root(c), "games/" + checked(t) + "/extracted"); }
    static File work(Context c, String t) { return new File(root(c), "builds/" + checked(t)); }
    static File code(Context c, String t) { return new File(root(c), "code/" + checked(t)); }
    static File state(Context c, String t) {
        // Generic games share the "generic" key; saves are separated per program ID and version.
        if (TitlePolicy.generic(checked(t))) {
            NativePackageFiles.Selection s = NativePackage.selection(c, t);
            return new File(root(c), "sessions/" + (s == null ? "generic-unprepared" : TitlePolicy.genericNamespace(s.programId, s.version)));
        }
        return new File(root(c), "sessions/" + checked(t) + "-1.0.0");
    }
    static File progress(Context c, String t) { return new File(work(c,t), "progress.json"); }
    static File cancel(Context c, String t) { return new File(work(c,t), "cancel.request"); }
    static File library(Context c, String t) throws IOException {
        File base=code(c,t), current=new File(base,"current");
        File directory=current.exists() ? current.getCanonicalFile() : base.getCanonicalFile();
        if (!directory.toPath().startsWith(base.getCanonicalFile().toPath())) throw new IOException("Invalid build publication path");
        return new File(directory,"liblasgame.so");
    }
    static boolean imported(Context c,String t) {
        return NativePackage.selection(c,t)!=null||extractedImported(c,t);
    }
    static boolean extractedImported(Context c,String t) {
        File g=game(c,t);
        if (!new File(g,"exefs/main").isFile() || !new File(g,"exefs/sdk").isFile()) return false;
        for(String module:TitlePolicy.get(t).modules)if(!new File(g,"exefs/"+module).isFile())return false;
        return new File(g,"romfs.bin").isFile() || new File(g,"romfs").isDirectory();
    }
    static boolean ready(Context c,String t) {
        NativePackageFiles.Selection selected=NativePackage.selection(c,t);
        if("continuous-nce".equals(new SettingsStore(c).getString(t,"cpu_backend"))&&selected!=null&&selected.reusable())return true;
        if(TitlePolicy.nativeOnly(t))return false;
        try { File lib=library(c,t); return extractedImported(c,t) && lib.isFile() && new File(lib.getPath()+".saotm").isFile(); }
        catch(IOException e) { return false; }
    }
    static JSONObject read(File path) {
        try { return new JSONObject(new String(Files.readAllBytes(path.toPath()),StandardCharsets.UTF_8)); }
        catch(Exception e) { return new JSONObject(); }
    }
    static String buildStatus(JSONObject progress) {
        if (progress==null || progress.length()==0) return "";
        String phase=progress.optString("state",progress.optString("phase","")).trim();
        switch (phase) {
            case "preparing": return "Preparing compiler tools";
            case "checking": return "Checking game and compiler files";
            case "generating": {
                String module=progress.optString("module","");
                switch (module) {
                    case "main": module="main program"; break;
                    case "sdk": module="system library"; break;
                    case "subsdk0": module="support library 1"; break;
                    case "subsdk1": module="support library 2"; break;
                }
                String text="Preparing game code"+(module.isEmpty()?"":" · "+module);
                long total=progress.optLong("total_functions",0);
                if (total>0) text+=" · "+Math.max(0,progress.optLong("generated_functions",0))+" / "+total+" functions";
                return text;
            }
            case "compiling": {
                long total=progress.optLong("total",0);
                return "Compiling game code"+(total>0?" · "+Math.max(0,progress.optLong("completed",0))+" / "+total+" files":"");
            }
            case "linking": return "Finishing compilation";
            case "verifying": return "Checking compiled game code";
            case "publishing": return "Saving compiled game code";
            case "complete": return "Compilation complete";
            case "cancelled":
            case "canceled": return "Cancelled · Resume when ready";
            case "failed": {
                String message=progress.optString("message","").trim();
                return message.isEmpty()?"Compilation failed":"Compilation failed: "+message;
            }
            default: return progress.optString("message","").trim();
        }
    }
    static String buildStatus(JSONObject progress,JSONObject active,String title) {
        String text=buildStatus(progress);
        int jobs=active.optInt("jobs",0);
        if (active.optBoolean("running") && title.equals(active.optString("title")) && jobs>=1 && jobs<=4 &&
                "compiling".equals(progress.optString("state",progress.optString("phase",""))))
            text+=" · "+jobs+(jobs==1?" compiler thread":" compiler threads");
        return text;
    }
    static void atomic(File path,String text) throws IOException {
        File parent=path.getParentFile();
        if (!parent.isDirectory() && !parent.mkdirs()) throw new IOException("Cannot create app folder");
        File temporary=new File(parent,"."+path.getName()+"-"+UUID.randomUUID());
        try(FileOutputStream stream=new FileOutputStream(temporary)) {
            stream.write(text.getBytes(StandardCharsets.UTF_8)); stream.getFD().sync();
        }
        if (!temporary.renameTo(path)) { temporary.delete(); throw new IOException("Cannot publish app file"); }
    }
    static void session(Context c,String t) throws IOException {
        checked(t);
        if (!ready(c,t)) throw new IOException("Import and prepare this game first");
        SettingsStore preferences=new SettingsStore(c);
        NativePackageFiles.Selection nativePackage="continuous-nce".equals(preferences.getString(t,"cpu_backend"))?NativePackage.selection(c,t):null;
        if(nativePackage!=null)NativePackage.verifyDocument(c,nativePackage);
        File canonical=state(c,t);
        File session=new File(canonical.getPath()+preferences.sessionSuffix(t)), save=new File(session,"save");
        if(!session.isDirectory()&&!session.mkdirs())throw new IOException("Cannot create isolated session");
        // Seed an experimental session once. Its subsequent saves remain independent.
        File canonicalSave=new File(canonical,"save");
        if(!session.equals(canonical)&&!save.exists()&&canonicalSave.isDirectory()) {
            File temporary=new File(session,".save-seed-"+UUID.randomUUID());
            try {copyPrivateTree(canonicalSave,temporary);if(!temporary.renameTo(save))throw new IOException("Cannot seed isolated save");}
            finally {GpuDrivers.deleteTree(temporary);}
        }
        if (!save.isDirectory() && !save.mkdirs()) throw new IOException("Cannot open isolated save folder");
        File settings=new File(session,"shared-baseline.ini");
        Map<String,String> ini=preferences.ini(t);
        // Eevee has no LAS frame-rate/aspect hooks. Retain its qualified baseline.
        if("eevee".equals(t)) { ini.put("frame_rate","2");ini.put("aspect","original");ini.put("blur_removal","0"); }
        File sourceSettings=settings.exists()?settings:new File(canonical,"shared-baseline.ini");
        atomic(settings,SettingsStore.merge(contents(sourceSettings),ini));
        File lib=library(c,t);
        Map<String,String> env=new LinkedHashMap<>(preferences.environment(t));
        env.put("LARECOMP_TITLE",t);env.put("LARECOMP_GAME_ROOT",nativePackage==null?game(c,t).getAbsolutePath():nativePackage.root.getAbsolutePath());
        env.put("LARECOMP_GAME_LIB",nativePackage==null?lib.getAbsolutePath():"");env.put("LARECOMP_SHARED_MANIFEST",nativePackage==null?lib.getAbsolutePath()+".saotm":"");
        // Every title switch replaces both setup paths and package credentials.
        env.put("LARECOMP_NATIVE_MANIFEST",nativePackage==null?"":nativePackage.manifest.getAbsolutePath());
        env.put("LARECOMP_PACKAGE_URI",nativePackage==null?"":nativePackage.uri);
        env.put("LARECOMP_PACKAGE_PATH","");
        env.put("LARECOMP_PACKAGE_KEYS",nativePackage==null?"":nativePackage.keys.getAbsolutePath());
        // Generic startup binds the prepared program/version. Other titles get no
        // entry at all: every LARECOMP_* line is shader-cache identity input.
        boolean generic=TitlePolicy.generic(t)&&nativePackage!=null;
        if(generic){env.put("LARECOMP_GENERIC_PROGRAM_ID",nativePackage.programId);env.put("LARECOMP_GENERIC_VERSION",nativePackage.version);}
        env.put("LARECOMP_NVN_DIAGNOSTIC_TRAPS",TitlePolicy.nativeOnly(t)?"1":"0");
        env.put("LARECOMP_SETTINGS",settings.getAbsolutePath());env.put("LARECOMP_NPAD","eevee".equals(t)?"handheld":"fullkey");
        env.put("LARECOMP_FONT_SCALE","1");env.put("LARECOMP_VK_DYN_VERTEX","0");env.put("LARECOMP_NO_LAUNCHER","1");
        // Retain Eevee's measured acquire workaround with either driver.
        // Driver selection belongs to the app, not an individual title.
        env.put("LARECOMP_VK_ACQUIRE_FENCE","eevee".equals(t)?"0":"1");
        // LAS requires an enabled custom driver for readable menu text on Thor.
        // Preserve the user's selection and GpuDrivers.config's failure marker.
        if ("las".equals(t) && !GpuDrivers.selected(c)) throw new IOException("Install and enable a graphics driver for Link’s Awakening first");
        File environment=new File(root(c),"env.txt");
        String previous=withoutResearchEnvironment(contents(environment));
        if(!generic)previous=withoutKeys(previous,new HashSet<>(Arrays.asList("LARECOMP_GENERIC_PROGRAM_ID","LARECOMP_GENERIC_VERSION")));
        atomic(environment,SettingsStore.mergeEnvironment(previous,env));
    }
    static String withoutResearchEnvironment(String original) {
        return withoutKeys(original,new HashSet<>(Arrays.asList("LARECOMP_AUDIO_WAV","LARECOMP_LOG_SKIPS","LARECOMP_SAVE_MSL",
            "LARECOMP_SAVE_BLOB","LARECOMP_SYNC_SHADERS","LARECOMP_HOST_INPUT","LARECOMP_INPUT",
            "LARECOMP_NO_USER_INPUT","LARECOMP_NCE_FAULT_TRACE","LARECOMP_MATERIAL_PROBE",
            "LARECOMP_GPU_UNIFORM_FORWARD","LARECOMP_VK_D24","LARECOMP_VK_EQUAL_TOL",
            "LARECOMP_VK_COPY_LAZY","LARECOMP_VK_COPY_OPT","LARECOMP_SYNC_FLUSH","LARECOMP_SYNC_LAG",
            "LARECOMP_GCOPY_PROBE","LARECOMP_VK_EXACT_H2F","LARECOMP_VK_PRESENT","LARECOMP_LOG_INTERVAL")));
    }
    private static String withoutKeys(String original,Set<String> keys) {
        StringBuilder out=new StringBuilder();for(String line:original.split("\r?\n",-1)) {
            int equals=line.indexOf('=');String key=equals<0?"":line.substring(0,equals).trim();
            if(!line.isEmpty()&&!keys.contains(key))out.append(line).append('\n');
        }return out.toString();
    }
    private static String contents(File path) throws IOException {
        return path.exists()?new String(Files.readAllBytes(path.toPath()),StandardCharsets.UTF_8):"";
    }
    private static void copyPrivateTree(File source,File destination) throws IOException {
        if(Files.isSymbolicLink(source.toPath()))throw new IOException("Cannot seed symbolic save link");
        if(source.isDirectory()) {
            if(!destination.mkdir())throw new IOException("Cannot create isolated save folder");
            File[] children=source.listFiles();if(children==null)throw new IOException("Cannot read save folder");
            for(File child:children)copyPrivateTree(child,new File(destination,child.getName()));
        } else if(source.isFile())Files.copy(source.toPath(),destination.toPath());
        else throw new IOException("Unsupported save entry");
    }
    private SharedFiles() {}
}
