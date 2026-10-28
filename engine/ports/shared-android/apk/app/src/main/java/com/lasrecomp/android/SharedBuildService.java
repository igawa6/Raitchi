package com.lasrecomp.android;

import android.app.*;
import android.content.*;
import android.content.pm.ServiceInfo;
import android.os.*;
import org.json.JSONObject;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.security.MessageDigest;
import java.util.*;
import java.util.zip.*;

/** Compiler runs outside both the library shell and the game process. */
public final class SharedBuildService extends Service {
    private static final int NOTIFICATION=5201;
    private static final String CHANNEL="shared-compilation";
    private volatile Thread worker;
    private volatile java.lang.Process compiler;
    private PowerManager.WakeLock wake;
    private String title;
    private final Handler handler=new Handler(Looper.getMainLooper());
    private final Runnable ticker=new Runnable() { public void run() {
        if(worker!=null && worker.isAlive()) {
            JSONObject p=SharedFiles.read(SharedFiles.progress(SharedBuildService.this,title));
            String progress=SharedFiles.buildStatus(p,SharedFiles.read(new File(getFilesDir(),"builds/active.json")),title);
            notifyBuild(progress.isEmpty()?"Preparing compilation":progress);
            handler.postDelayed(this,1500);
        }
    }};
    @Override public void onCreate() {
        super.onCreate(); NotificationManager nm=getSystemService(NotificationManager.class);
        nm.createNotificationChannel(new NotificationChannel(CHANNEL,"Game compilation",NotificationManager.IMPORTANCE_LOW));
    }
    @Override public int onStartCommand(Intent intent,int flags,int startId) {
        if(worker!=null && worker.isAlive()) return START_NOT_STICKY;
        try { title=SharedFiles.checked(intent==null?null:intent.getStringExtra("title")); }
        catch(Exception e) { stopSelf(); return START_NOT_STICKY; }
        startForeground(NOTIFICATION,notification("Preparing compiler"),ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC);
        worker=new Thread(()->build(title),"shared-game-build"); worker.start(); handler.post(ticker);
        return START_NOT_STICKY;
    }
    private void build(String t) {
        try {
            if(SharedLauncherActivity.gamePid(this)!=0) throw new IOException("End the running game before compiling");
            if(TitlePolicy.nativeOnly(t))throw new IOException("This title uses native game-file preparation");
            if(!SharedFiles.extractedImported(this,t)) throw new IOException("Import the extracted game files first");
            File work=SharedFiles.work(this,t); if(!work.isDirectory() && !work.mkdirs()) throw new IOException("Cannot create build folder");
            if(SharedFiles.cancel(this,t).exists() && !SharedFiles.cancel(this,t).delete()) throw new IOException("Cannot clear previous cancellation");
            status("preparing","Preparing compiler tools");
            SharedFiles.atomic(new File(getFilesDir(),"builds/active.json"),new JSONObject().put("title",t).put("running",true).put("pid",android.os.Process.myPid()).toString());
            wake=getSystemService(PowerManager.class).newWakeLock(PowerManager.PARTIAL_WAKE_LOCK,"SharedRecomp:compile");
            wake.setReferenceCounted(false); wake.acquire(6*60*60*1000L);
            android.os.Process.setThreadPriority(android.os.Process.THREAD_PRIORITY_BACKGROUND);
            File kit=prepareKit();
            File launcher=prepareParallelLauncher();
            JSONObject config=SharedFiles.read(new File(kit,"shared-build-kit.json"));
            String nativeDir=getApplicationInfo().nativeLibraryDir;
            ActivityManager.MemoryInfo memory=new ActivityManager.MemoryInfo();
            getSystemService(ActivityManager.class).getMemoryInfo(memory);
            int cpus=Math.max(1,Runtime.getRuntime().availableProcessors());
            int jobs=Math.min(cpus,memory.lowMemory || memory.availMem<(1L<<30)?1:
                memory.totalMem>=(12L<<30) && memory.availMem>=(3L<<30)?4:2);
            SharedFiles.atomic(new File(getFilesDir(),"builds/active.json"),new JSONObject().put("title",t).put("running",true)
                .put("pid",android.os.Process.myPid()).put("jobs",jobs).put("cpu_count",cpus)
                .put("memory_total_bytes",memory.totalMem).put("memory_available_bytes",memory.availMem)
                .put("memory_low",memory.lowMemory).toString());
            ArrayList<String> args=new ArrayList<>(Arrays.asList(new File(nativeDir,"liblaspython.so").getPath(),
                launcher.getPath(),"--producer",new File(kit,"shared_ondevice_build.py").getPath(),"--title",t,"--game-root",SharedFiles.game(this,t).getPath(),
                "--work",work.getPath(),"--output",SharedFiles.code(this,t).getPath(),"--kit",kit.getPath(),"--native-lib-dir",nativeDir,
                "--progress",SharedFiles.progress(this,t).getPath(),"--cancel-file",SharedFiles.cancel(this,t).getPath(),"--jobs",Integer.toString(jobs)));
            ProcessBuilder builder=new ProcessBuilder(args).directory(work).redirectErrorStream(true);
            Map<String,String> env=builder.environment(); env.put("LD_LIBRARY_PATH",nativeDir); env.put("LIBCAPSTONE_PATH",nativeDir); env.put("LIBUNICORN_PATH",nativeDir);
            env.put("LASPY_HOME",inside(kit,config.getString("python_home")).getPath());
            ArrayList<String> search=new ArrayList<>();
            for(int i=0;i<config.getJSONArray("python_paths").length();i++) search.add(inside(kit,config.getJSONArray("python_paths").getString(i)).getPath());
            env.put("LASPY_PATH",String.join(":",search)); env.put("TMPDIR",work.getPath());
            builder.redirectOutput(new File(work,"build.log"));
            compiler=builder.start(); int result=compiler.waitFor(); compiler=null;
            if(result!=0) {
                JSONObject p=SharedFiles.read(SharedFiles.progress(this,t));
                String detail=p.optString("message", "");
                if(SharedFiles.cancel(this,t).exists()) status("cancelled","Cancelled; completed objects are kept. Resume when ready.");
                else throw new IOException(detail.isEmpty()?"Compilation failed ("+result+"). Build log is preserved.":detail);
            } else if(!SharedFiles.ready(this,t)) throw new IOException("Compiler did not publish a complete game library");
            else status("complete","Ready to play");
        } catch(Exception e) {
            try { status("failed",e.getMessage()==null?e.toString():e.getMessage()); } catch(Exception ignored) {}
        } finally {
            try { SharedFiles.atomic(new File(getFilesDir(),"builds/active.json"),new JSONObject().put("title",t).put("running",false).toString()); } catch(Exception ignored) {}
            if(wake!=null && wake.isHeld()) wake.release(); handler.removeCallbacks(ticker); stopForeground(STOP_FOREGROUND_REMOVE); stopSelf();
        }
    }
    private void status(String state,String message) throws Exception {
        JSONObject p=SharedFiles.read(SharedFiles.progress(this,title));
        SharedFiles.atomic(SharedFiles.progress(this,title),p.put("state",state).put("message",message).toString());
    }
    private File prepareParallelLauncher() throws Exception {
        JSONObject meta;
        try(InputStream in=getAssets().open("shared-parallel-build.json")) {
            meta=new JSONObject(new String(in.readAllBytes(),StandardCharsets.UTF_8));
        }
        String expected=meta.getString("sha256");
        if(meta.optInt("schema")!=1 || !expected.matches("[0-9a-f]{64}")) throw new IOException("Invalid compiler launcher metadata");
        byte[] source;
        try(InputStream in=getAssets().open("shared_parallel_build.py")) { source=in.readAllBytes(); }
        if(!hex(MessageDigest.getInstance("SHA-256").digest(source)).equals(expected)) throw new IOException("Compiler launcher checksum differs");
        File path=new File(getFilesDir(),"builds/launcher/shared_parallel_build-"+expected+".py");
        if(!path.isFile() || !hex(MessageDigest.getInstance("SHA-256").digest(Files.readAllBytes(path.toPath()))).equals(expected))
            SharedFiles.atomic(path,new String(source,StandardCharsets.UTF_8));
        if(!hex(MessageDigest.getInstance("SHA-256").digest(Files.readAllBytes(path.toPath()))).equals(expected))
            throw new IOException("Cannot verify compiler launcher");
        return path;
    }
    private File prepareKit() throws Exception {
        JSONObject meta;
        try(InputStream in=getAssets().open("ondevice-manifest.json")) { meta=new JSONObject(new String(in.readAllBytes(),StandardCharsets.UTF_8)); }
        String expected=meta.getString("archive_sha256");
        File kit=new File(getFilesDir(),"ondevice"), marker=new File(kit,".archive-sha256");
        if(marker.isFile() && new String(Files.readAllBytes(marker.toPath()),StandardCharsets.UTF_8).trim().equals(expected)) return kit;
        File temporary=new File(getFilesDir(),".ondevice-"+UUID.randomUUID());
        boolean installed=false;
        try {
            if(!temporary.mkdirs()) throw new IOException("Cannot prepare compiler folder");
            if(temporary.getUsableSpace()<(1L<<30)) throw new IOException("At least 1 GB free storage is needed for compiler tools");
            MessageDigest sha=MessageDigest.getInstance("SHA-256"); long total=0; int entries=0;
            try(InputStream raw=getAssets().open("ondevice.zip");java.security.DigestInputStream hashed=new java.security.DigestInputStream(raw,sha);ZipInputStream zip=new ZipInputStream(hashed)) {
                ZipEntry entry; byte[] buffer=new byte[1<<20];
                while((entry=zip.getNextEntry())!=null) {
                    if(SharedFiles.cancel(this,title).exists()) throw new IOException("Compilation cancelled");
                    if(++entries>50000) throw new IOException("Compiler archive contains too many files");
                    File path=inside(temporary,entry.getName());
                    if(entry.isDirectory()) { if(!path.isDirectory() && !path.mkdirs()) throw new IOException("Cannot create compiler folder"); continue; }
                    if(!path.getParentFile().isDirectory() && !path.getParentFile().mkdirs()) throw new IOException("Cannot create compiler folder");
                    try(FileOutputStream out=new FileOutputStream(path)) { int n; while((n=zip.read(buffer))!=-1) {
                        total+=n; if(total>(2L<<30)) throw new IOException("Compiler archive is too large"); out.write(buffer,0,n);
                    }}
                }
                // Include the central directory and any trailer in the archive digest.
                while(hashed.read(buffer)!=-1) {}
            }
            if(!hex(sha.digest()).equals(expected)) throw new IOException("Compiler archive checksum differs");
            if(!new File(temporary,"shared-build-kit.json").isFile()) throw new IOException("Incomplete compiler toolkit");
            SharedFiles.atomic(new File(temporary,".archive-sha256"),expected+"\n");
            File previous=new File(getFilesDir(),".ondevice-previous-"+UUID.randomUUID());
            if(kit.exists() && !kit.renameTo(previous)) throw new IOException("Cannot retain previous compiler tools");
            if(!temporary.renameTo(kit)) { if(previous.exists()) previous.renameTo(kit); throw new IOException("Cannot install compiler tools"); }
            installed=true;
            delete(previous); return kit;
        } finally {
            // Any failure above (checksum mismatch, cancellation, oversized archive, no
            // space) otherwise left this scratch extraction directory on disk forever.
            if(!installed) delete(temporary);
        }
    }
    private static File inside(File base,String relative) throws IOException {
        if(relative.startsWith("/") || relative.contains("\\")) throw new IOException("Invalid compiler path");
        File path=new File(base,relative).getCanonicalFile();
        if(!path.toPath().startsWith(base.getCanonicalFile().toPath()) || path.equals(base.getCanonicalFile())) throw new IOException("Invalid compiler path");
        return path;
    }
    private static String hex(byte[] bytes) { StringBuilder out=new StringBuilder();for(byte b:bytes)out.append(String.format(java.util.Locale.ROOT,"%02x",b&255));return out.toString(); }
    private static void delete(File path) { if(path.isDirectory()) { File[] children=path.listFiles(); if(children!=null)for(File child:children)delete(child); } path.delete(); }
    private Notification notification(String text) {
        PendingIntent open=PendingIntent.getActivity(this,0,new Intent(this,SharedLauncherActivity.class),PendingIntent.FLAG_IMMUTABLE);
        return new Notification.Builder(this,CHANNEL).setSmallIcon(android.R.drawable.stat_sys_download).setContentTitle("Compiling "+SharedFiles.label(title))
            .setContentText(text).setOngoing(true).setOnlyAlertOnce(true).setContentIntent(open).setProgress(0,0,true).build();
    }
    private void notifyBuild(String text) { getSystemService(NotificationManager.class).notify(NOTIFICATION,notification(text)); }
    @Override public void onDestroy() {
        handler.removeCallbacks(ticker);
        if(worker!=null && worker.isAlive()) {
            try { SharedFiles.atomic(SharedFiles.cancel(this,title),"cancel\n"); } catch(Exception ignored) {}
            java.lang.Process p=compiler; if(p!=null) p.destroy();
        }
        if(wake!=null && wake.isHeld()) wake.release(); super.onDestroy();
    }
    @Override public void onTimeout(int startId,int type) { stopSelf(); }
    @Override public IBinder onBind(Intent i) { return null; }
}
