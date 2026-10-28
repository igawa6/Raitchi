package com.lasrecomp.android;
import android.content.Context;
import android.content.SharedPreferences;
import java.util.*;
import java.io.*;
import java.nio.file.*;

/** Runs production SettingsStore with an explicit in-memory Android preference stand-in. */
public final class RaitchiSettingsTest {
    private static int checks;
    private static void check(boolean ok) { checks++;if(!ok)throw new AssertionError("check "+checks); }
    private static final class Prefs implements SharedPreferences {
        final Map<String,String> data=new HashMap<>();boolean writable=true;
        public String getString(String key,String fallback){return data.getOrDefault(key,fallback);}
        public boolean contains(String key){return data.containsKey(key);}
        public Editor edit(){return new Editor(){final Map<String,String> updates=new HashMap<>();final Set<String> removed=new HashSet<>();
            public Editor putString(String key,String value){updates.put(key,value);return this;}
            public Editor remove(String key){removed.add(key);return this;}
            public boolean commit(){if(!writable)return false;for(String key:removed)data.remove(key);data.putAll(updates);return true;}
        };}
    }
    private static final class App extends Context {
        final Prefs preferences=new Prefs();
        File files;
        public SharedPreferences getSharedPreferences(String name,int mode){check(name.equals("raitchi_settings"));return preferences;}
        public File getFilesDir(){return files;}
    }
    private static void write(File file,String content) throws Exception {
        file.getParentFile().mkdirs();Files.writeString(file.toPath(),content);
    }
    private static String read(File file) throws Exception {return Files.readString(file.toPath());}
    private static void sessions() throws Exception {
        App app=new App();app.files=Files.createTempDirectory(Path.of(System.getProperty("raitchi.test.root")),"private-dummy-session-").toFile();
        SettingsStore store=new SettingsStore(app);
        for(String title:SharedFiles.TITLES) {
            for(String module:new String[]{"main","sdk","subsdk0","subsdk1"})write(new File(SharedFiles.game(app,title),"exefs/"+module),"synthetic test input, not a game");
            write(new File(SharedFiles.game(app,title),"romfs.bin"),"synthetic");
            write(new File(SharedFiles.code(app,title),"liblasgame.so"),"synthetic");
            write(new File(SharedFiles.code(app,title),"liblasgame.so.saotm"),"synthetic");
        }
        File canonical=SharedFiles.state(app,"las"),canonicalSettings=new File(canonical,"shared-baseline.ini");
        write(new File(canonical,"save/nested/slot"),"canonical-save");
        write(canonicalSettings,"# keep canonical\nvolume = 0.25\nunknown_ini = keep\n");
        File environment=new File(app.files,"env.txt");write(environment,"LARECOMP_TRACE=keep\nLARECOMP_VK_ACQUIRE_FENCE=0\nLARECOMP_NATIVE_MANIFEST=/previous-title.native\nLARECOMP_PACKAGE_PATH=/old-owner-game.nsp\nLARECOMP_MATERIAL_PROBE=1\nLARECOMP_AUDIO_WAV=/prior.wav\nLARECOMP_LOG_SKIPS=\nLARECOMP_INPUT=script\nLARECOMP_NVN_DIAGNOSTIC_TRAPS=1\n");
        store.setString("las","cpu_backend","continuous-nce");store.setString("las","volume","0.50");
        SharedFiles.session(app,"las");
        File nativeState=new File(canonical.getPath()+"-nce"),nativeSave=new File(nativeState,"save/nested/slot");
        check(read(nativeSave).equals("canonical-save"));
        check(read(new File(canonical,"save/nested/slot")).equals("canonical-save"));
        check(read(canonicalSettings).equals("# keep canonical\nvolume = 0.25\nunknown_ini = keep\n"));
        String settings=read(new File(nativeState,"shared-baseline.ini"));
        check(settings.contains("volume = 0.50\n"));check(settings.contains("unknown_ini = keep\n"));
        String env=read(environment);check(env.contains("LARECOMP_TRACE=keep\n"));check(env.contains("LARECOMP_VK_ACQUIRE_FENCE=1\n"));
        check(env.contains("LARECOMP_SETTINGS="+new File(nativeState,"shared-baseline.ini").getAbsolutePath()+"\n"));
        check(env.contains("LARECOMP_CPU_BACKEND=continuous-nce\n"));
        check(!env.contains("LARECOMP_AUDIO_WAV="));check(!env.contains("LARECOMP_LOG_SKIPS="));check(!env.contains("LARECOMP_INPUT="));
        check(env.contains("LARECOMP_NVN_DIAGNOSTIC_TRAPS=0\n"));check(env.contains("LARECOMP_PACKAGE_PATH=\n"));check(!env.contains("/old-owner-game.nsp"));check(!env.contains("LARECOMP_MATERIAL_PROBE="));
        write(nativeSave,"independent-native-save");SharedFiles.session(app,"las");check(read(nativeSave).equals("independent-native-save"));
        SharedFiles.session(app,"eevee");env=read(environment);
        check(env.contains("LARECOMP_NATIVE_MANIFEST=\n"));check(!env.contains("/previous-title.native"));
        check(env.contains("LARECOMP_CPU_BACKEND=continuous-nce\n"));check(env.contains("LARECOMP_NPAD=handheld\n"));check(env.contains("LARECOMP_VK_ACQUIRE_FENCE=0\n"));
        SharedFiles.session(app,"las");check(read(environment).contains("LARECOMP_VK_ACQUIRE_FENCE=1\n"));
        store.setString("las","cpu_backend","aot");SharedFiles.session(app,"las");
        check(read(canonicalSettings).contains("volume = 0.50\n"));check(read(new File(canonical,"save/nested/slot")).equals("canonical-save"));
        check(read(environment).contains("LARECOMP_SETTINGS="+canonicalSettings.getAbsolutePath()+"\n"));
        check(read(nativeSave).equals("independent-native-save"));
        check(read(environment).contains("LARECOMP_TRACE=keep\n"));
        // Native package selection uses independent metadata, never an old DSO.
        for(String title:new String[]{"echoes","wonder"}){
        TitlePolicy policy=TitlePolicy.get(title);
        File stage=NativePackageFiles.begin(app.files,title);
        byte[] nativeManifest=new byte[128+104*policy.modules.length];System.arraycopy(new byte[]{'N','A','T','V','M','0','0','1'},0,nativeManifest,0,8);
        java.nio.ByteBuffer manifest=java.nio.ByteBuffer.wrap(nativeManifest).order(java.nio.ByteOrder.LITTLE_ENDIAN);
        manifest.putInt(8,nativeManifest.length);manifest.putInt(12,1);manifest.putInt(16,policy.id);manifest.putInt(20,policy.modules.length);
        Files.write(new File(stage,"native.manifest").toPath(),nativeManifest);write(new File(stage,"prod.keys"),"synthetic");
        byte[] nso=new byte[256];nso[0]='N';nso[1]='S';nso[2]='O';nso[3]='0';
        for(String module:policy.modules){File f=new File(stage,"exefs/"+module);f.getParentFile().mkdirs();Files.write(f.toPath(),nso);}
        NativePackageFiles.Selection selected=NativePackageFiles.publish(app.files,title,stage,"content://synthetic/owner.nsp");
        check(SharedFiles.imported(app,title));check(SharedFiles.ready(app,title));
        SharedFiles.session(app,title);env=read(environment);
        check(env.contains("LARECOMP_TITLE="+title+"\n"));check(env.contains("LARECOMP_NATIVE_MANIFEST="+selected.manifest.getAbsolutePath()+"\n"));
        check(env.contains("LARECOMP_NVN_DIAGNOSTIC_TRAPS=1\n"));
        check(env.contains("LARECOMP_GAME_ROOT="+selected.root.getAbsolutePath()+"\n"));check(env.contains("LARECOMP_GAME_LIB=\n"));check(env.contains("LARECOMP_SHARED_MANIFEST=\n"));
        check(env.contains("LARECOMP_PACKAGE_PATH=\n"));check(env.contains("LARECOMP_PACKAGE_URI=content://synthetic/owner.nsp\n"));check(env.contains("LARECOMP_PACKAGE_KEYS="+selected.keys.getAbsolutePath()+"\n"));
        check(new File(SharedFiles.state(app,title).getPath()+"-nce/save").isDirectory());
        SharedFiles.session(app,"eevee");env=read(environment);
        check(env.contains("LARECOMP_PACKAGE_URI=\n"));check(env.contains("LARECOMP_PACKAGE_KEYS=\n"));check(env.contains("LARECOMP_NATIVE_MANIFEST=\n"));
        check(!env.contains(selected.keys.getAbsolutePath()));check(!env.contains(selected.root.getAbsolutePath()));
        }
        // Generic startup: same native session path, program/version-specific save namespace, no LAS/Eevee tweaks.
        check(!SharedFiles.imported(app,"generic")&&!SharedFiles.ready(app,"generic"));
        boolean refused=false;try{SharedFiles.session(app,"generic");}catch(IOException expected){refused=true;}check(refused);
        File stage=NativePackageFiles.begin(app.files,"generic");
        byte[] nativeManifest=new byte[128+104*3];System.arraycopy(new byte[]{'N','A','T','V','M','0','0','1'},0,nativeManifest,0,8);
        java.nio.ByteBuffer manifest=java.nio.ByteBuffer.wrap(nativeManifest).order(java.nio.ByteOrder.LITTLE_ENDIAN);
        manifest.putInt(8,nativeManifest.length);manifest.putInt(12,1);manifest.putInt(16,5);manifest.putInt(20,3);manifest.putLong(24,0x0100aaaabbbb0000L);
        manifest.putInt(128,1);manifest.putInt(232,3);manifest.putInt(336,2);
        Files.write(new File(stage,"native.manifest").toPath(),nativeManifest);write(new File(stage,"prod.keys"),"synthetic");
        byte[] nso=new byte[256];nso[0]='N';nso[1]='S';nso[2]='O';nso[3]='0';
        for(String module:new String[]{"main","subsdk0","sdk"}){File f=new File(stage,"exefs/"+module);f.getParentFile().mkdirs();Files.write(f.toPath(),nso);}
        NativePackageFiles.Selection generic=NativePackageFiles.publish(app.files,"generic",stage,"content://synthetic/unknown.nsp","0100aaaabbbb0000","1.2.0");
        check(SharedFiles.imported(app,"generic"));check(SharedFiles.ready(app,"generic"));
        check(store.getString("generic","cpu_backend").equals("continuous-nce")&&store.sessionSuffix("generic").equals("-nce"));
        File genericState=SharedFiles.state(app,"generic");
        check(genericState.equals(new File(app.files,"sessions/generic-0100aaaabbbb0000-1.2.0")));
        File genericPointer=new File(app.files,"games/generic/native/selection.properties");
        String currentPointer=read(genericPointer);java.util.Properties stalePolicy=new java.util.Properties();
        try(java.io.InputStream in=new java.io.FileInputStream(genericPointer)){stalePolicy.load(in);}
        stalePolicy.remove("preparationPolicy");try(java.io.OutputStream out=new java.io.FileOutputStream(genericPointer)){stalePolicy.store(out,"");}
        check(SharedFiles.imported(app,"generic")&&!SharedFiles.ready(app,"generic"));
        check(SharedFiles.state(app,"generic").equals(genericState));
        refused=false;try{SharedFiles.session(app,"generic");}catch(IOException expected){refused=true;}check(refused);
        write(genericPointer,currentPointer);check(SharedFiles.ready(app,"generic"));
        SharedFiles.session(app,"generic");env=read(environment);
        check(env.contains("LARECOMP_TITLE=generic\n"));check(env.contains("LARECOMP_NATIVE_MANIFEST="+generic.manifest.getAbsolutePath()+"\n"));
        check(env.contains("LARECOMP_GENERIC_PROGRAM_ID=0100aaaabbbb0000\n"));check(env.contains("LARECOMP_GENERIC_VERSION=1.2.0\n"));
        check(env.contains("LARECOMP_CPU_BACKEND=continuous-nce\n"));check(env.contains("LARECOMP_NVN_DIAGNOSTIC_TRAPS=1\n"));
        check(env.contains("LARECOMP_NPAD=fullkey\n"));check(env.contains("LARECOMP_VK_ACQUIRE_FENCE=1\n"));
        check(env.contains("LARECOMP_GAME_ROOT="+generic.root.getAbsolutePath()+"\n"));check(env.contains("LARECOMP_GAME_LIB=\n"));
        check(env.contains("LARECOMP_PACKAGE_URI=content://synthetic/unknown.nsp\n"));check(env.contains("LARECOMP_PACKAGE_KEYS="+generic.keys.getAbsolutePath()+"\n"));
        check(new File(genericState.getPath()+"-nce/save").isDirectory());
        String ini=read(new File(genericState.getPath()+"-nce/shared-baseline.ini"));
        check(!ini.contains("frame_rate")&&!ini.contains("aspect")&&!ini.contains("blur_removal"));
        SharedFiles.session(app,"eevee");env=read(environment);
        check(!env.contains("LARECOMP_GENERIC_"));check(!env.contains("0100aaaabbbb0000"));
        // Supported titles never receive generic entries, so their shader-cache identity input is unchanged.
        SharedFiles.session(app,"las");check(!read(environment).contains("LARECOMP_GENERIC_"));
    }
    public static void main(String[] args) throws Exception {
        App app=new App();SettingsStore store=new SettingsStore(app);
        check(store.getString("las","cpu_backend").equals("continuous-nce"));
        check(store.getString("eevee","render_scale").equals("0.6666666666666666"));
        store.setString(null,"volume","0.50");
        check(store.getString("las","volume").equals("0.50"));check(store.getString("eevee","volume").equals("0.50"));
        store.setString("las","volume","0.25");store.setString(null,"volume","0.75");
        check(store.hasOverride("las","volume"));check(!store.hasOverride("eevee","volume"));
        check(new SettingsStore(app).getString("las","volume").equals("0.25"));
        check(store.getString("eevee","volume").equals("0.75"));
        store.clearOverride("las","volume");check(store.getString("las","volume").equals("0.75"));
        store.setBoolean("las","rumble",false);check(!store.getBoolean("las","rumble"));check(store.getBoolean("eevee","rumble"));
        store.setString("las","cpu_backend","continuous-nce");check(store.environment("las").get("LARECOMP_CPU_BACKEND").equals("continuous-nce"));
        check(store.sessionSuffix("las").equals("-nce"));check(store.sessionSuffix("eevee").equals("-nce"));
        check(store.environment("eevee").get("LARECOMP_CPU_BACKEND").equals("continuous-nce"));
        store.setString(null,"cpu_backend","aot");
        check(store.getString("echoes","cpu_backend").equals("continuous-nce"));
        check(store.sessionSuffix("echoes").equals("-nce"));
        check(store.getString("wonder","cpu_backend").equals("continuous-nce"));check(store.sessionSuffix("wonder").equals("-nce"));
        boolean wonderAot=false;try{store.setString("wonder","cpu_backend","aot");}catch(IllegalArgumentException expected){wonderAot=true;}check(wonderAot);
        boolean echoesAot=false;try{store.setString("echoes","cpu_backend","aot");}catch(IllegalArgumentException expected){echoesAot=true;}check(echoesAot);
        check(store.environment("eevee").get("LARECOMP_CPU_BACKEND").equals("aot"));
        check(store.environment("las").get("LARECOMP_CPU_BACKEND").equals("continuous-nce"));
        check(store.sessionSuffix("eevee").isEmpty());
        check(!store.ini("eevee").containsKey("frame_rate"));check(store.ini("las").get("frame_rate").equals("2"));
        store.setString("las","render_scale","0");check(store.ini("las").get("resolution").equals("dynamic"));
        check(store.environment("las").get("LARECOMP_SCALE").equals("0"));
        String ini=SettingsStore.merge("# retained\nvolume = 0.99\nvolume=0.40\nunknown = keep\n",store.ini("las"));
        check(ini.contains("# retained\n"));check(ini.contains("unknown = keep\n"));
        check(ini.indexOf("volume") == ini.lastIndexOf("volume"));check(ini.contains("volume = 0.75\n"));
        check(SettingsStore.merge(ini,store.ini("las")).equals(ini));
        Map<String,String> env=store.environment("las");
        String merged=SettingsStore.mergeEnvironment("LARECOMP_CPU_BACKEND=native-blocks\nLARECOMP_TRACE=keep\n# comment\n",env);
        check(merged.contains("LARECOMP_TRACE=keep\n"));check(merged.contains("LARECOMP_CPU_BACKEND=continuous-nce\n"));
        check(!merged.contains("LARECOMP_CPU_BACKEND ="));check(SettingsStore.mergeEnvironment(merged,env).equals(merged));
        store.reset("las");check(store.getString("las","cpu_backend").equals("aot"));check(store.getString("las","volume").equals("0.75"));
        check(!store.hasOverride("las","rumble"));
        store.reset(null);check(store.getString("las","volume").equals("1.00"));
        app.preferences.data.put("global/cpu_backend","not-a-backend");check(store.getString("las","cpu_backend").equals("continuous-nce"));
        app.preferences.data.put("game/las/volume","injected\ntext");check(store.getString("las","volume").equals("1.00"));
        for(String[] bad:new String[][]{{"las","volume","-1"},{"las","cpu_backend","unknown"},{"unsupported","volume","1.00"},{"las","not-key","1"}}) {
            boolean rejected=false;try{store.setString(bad[0],bad[1],bad[2]);}catch(IllegalArgumentException expected){rejected=true;}check(rejected);
        }
        for(SettingsStore.Option option:SettingsStore.OPTIONS) {
            check(option.values.length==option.labels.length);check(option.accepts(option.initial));
            for(String value:option.values) {store.setString("las",option.key,value);check(store.getString("las",option.key).equals(value));}
        }
        app.preferences.writable=false;boolean failure=false;
        try{store.setString("las","volume","0.50");}catch(IllegalStateException expected){failure=true;}check(failure);
        sessions();
        System.out.println(checks+" settings assertions passed (controlled SharedPreferences stand-in)");
    }
}
