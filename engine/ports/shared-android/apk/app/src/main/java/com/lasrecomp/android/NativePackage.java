package com.lasrecomp.android;

import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import java.io.*;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicLong;
import org.json.JSONObject;

/** Native package preparation owns application context, never an Activity or renderer. */
final class NativePackage {
    private static final AtomicBoolean running=new AtomicBoolean();
    private static volatile String status="";
    private static final AtomicLong revision=new AtomicLong();
    static native String prepare(int fd,String keysPath,String outputRoot,String expectedTitle)throws IOException;
    static boolean isRunning(){return running.get();}
    static String status(){return status;}
    static long revision(){return revision.get();}
    static NativePackageFiles.Selection selection(Context c,String t){try{return NativePackageFiles.current(c.getFilesDir(),t);}catch(IOException e){return null;}}
    static void verifyDocument(Context c,NativePackageFiles.Selection s)throws IOException {
        try(ParcelFileDescriptor fd=c.getContentResolver().openFileDescriptor(Uri.parse(s.uri),"r")){if(fd==null)throw new IOException("Game file is unavailable. Import it again.");}
        catch(SecurityException e){throw new IOException("Game-file permission expired. Import it again.");}
    }
    static void start(Context owner,String title,Uri document,Uri keys)throws IOException {
        final Context c=owner.getApplicationContext();SharedFiles.checked(title);
        if(!running.compareAndSet(false,true))throw new IOException("A game is already being prepared");
        status="Preparing "+SharedFiles.label(title)+" from the game file…";
        revision.incrementAndGet();
        try {
            try {
                c.getContentResolver().takePersistableUriPermission(document,Intent.FLAG_GRANT_READ_URI_PERMISSION);
            } catch (SecurityException missingDirectGrant) {
                // Scanned documents inherit the persistable permission of their SAF tree.
                boolean retained = false;
                if (android.provider.DocumentsContract.isTreeUri(document)) {
                    String tree = android.provider.DocumentsContract.getTreeDocumentId(document);
                    for (android.content.UriPermission permission : c.getContentResolver().getPersistedUriPermissions()) {
                        Uri granted = permission.getUri();
                        if (permission.isReadPermission() && document.getAuthority().equals(granted.getAuthority()) &&
                                android.provider.DocumentsContract.isTreeUri(granted) && tree.equals(
                                android.provider.DocumentsContract.getTreeDocumentId(granted))) { retained = true; break; }
                    }
                }
                if (!retained) throw missingDirectGrant;
            }
            new Thread(()->{
                File stage=null;
                try {
                    stage=NativePackageFiles.begin(c.getFilesDir(),title);File privateKeys=new File(stage,"prod.keys");
                    try(InputStream in=c.getContentResolver().openInputStream(keys);FileOutputStream out=new FileOutputStream(privateKeys)) {
                        if(in==null)throw new IOException("The keys file is unavailable");byte[] buffer=new byte[8192];long total=0;int n;
                        while((n=in.read(buffer))!=-1){total+=n;if(total>(8L<<20))throw new IOException("Keys file is too large");out.write(buffer,0,n);}out.getFD().sync();
                    }
                    System.loadLibrary("raitchiprepare");
                    String program="",version="";
                    try(ParcelFileDescriptor fd=c.getContentResolver().openFileDescriptor(document,"r")) {
                        if(fd==null)throw new IOException("Game file is unavailable");
                        JSONObject result=new JSONObject(prepare(fd.getFd(),privateKeys.getAbsolutePath(),stage.getAbsolutePath(),title));
                        if(!title.equals(result.optString("title")))throw new IOException("Prepared game identity differs");
                        // Generic startup: the engine reports the program ID and save-version token it prepared.
                        if(TitlePolicy.generic(title)){program=result.optString("programId");version=result.optString("version");}
                    }
                    NativePackageFiles.publish(c.getFilesDir(),title,stage,document.toString(),program,version);stage=null;
                    new SettingsStore(c).setString(title,"cpu_backend","continuous-nce");
                    status=SharedFiles.label(title)+" is ready to play. Game data stays in the selected file.";
                }catch(Exception|LinkageError error){status="Preparation stopped: "+(error.getMessage()==null?"Cannot prepare this game file":error.getMessage());}
                finally{NativePackageFiles.discard(c.getFilesDir(),title,stage);revision.incrementAndGet();running.set(false);}
            },"raitchi-native-package").start();
        }catch(RuntimeException error){status="Cannot retain game-file access. Select a local NSP/XCI document.";revision.incrementAndGet();running.set(false);throw new IOException(status,error);}
    }
    private NativePackage(){}
}
