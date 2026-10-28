package com.lasrecomp.android;

import android.content.Context;
import android.net.Uri;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicLong;

/** One installation per launcher process; workers never retain an Activity. */
final class RaitchiUiDriverImport {
    interface Installer { String install() throws Exception; }
    private static final AtomicBoolean running=new AtomicBoolean();
    private static final AtomicLong revision=new AtomicLong();
    private static volatile String status="";
    static boolean isRunning(){return running.get();}
    static long revision(){return revision.get();}
    static String status(){return status;}
    static boolean start(Context context,Uri uri){
        final Context app=context.getApplicationContext();
        return begin(()->"Installed "+GpuDrivers.install(app,uri));
    }
    static boolean begin(Installer installer){
        if(!running.compareAndSet(false,true))return false;
        status="Installing graphics driver…";revision.incrementAndGet();
        Thread worker=new Thread(()->{
            String result="Driver installation stopped.";
            try{result=installer.install();}
            catch(Exception error){result="Driver installation failed: "+error.getMessage();}
            finally{status=result;running.set(false);revision.incrementAndGet();}
        },"raitchi-driver-import");
        try{worker.start();}
        catch(RuntimeException|Error error){status="Cannot start driver installation";running.set(false);revision.incrementAndGet();throw error;}
        return true;
    }
    private RaitchiUiDriverImport(){}
}
