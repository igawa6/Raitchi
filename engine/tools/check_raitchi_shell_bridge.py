#!/usr/bin/env python3
"""Host regression for the production Raitchi shell bridge with explicit Android/SAF stand-ins."""
import argparse
import hashlib
import json
import shutil
import subprocess
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
BUILD=ROOT/"build/raitchi-shell-bridge"
JAVA=ROOT/"ports/shared-android/apk/app/src/main/java/com/lasrecomp/android"

STUBS={
"android/content/SharedPreferences.java":'''package android.content;
public interface SharedPreferences {
 String getString(String key,String fallback); boolean contains(String key); Editor edit();
 interface Editor { Editor putString(String key,String value); Editor remove(String key); boolean commit(); }
}''',
"android/content/ContentResolver.java":'''package android.content;
import android.database.Cursor; import android.net.Uri; import java.io.*;
public abstract class ContentResolver { public abstract Cursor query(Uri uri,String[] columns,String selection,String[] args,String order); public abstract InputStream openInputStream(Uri uri) throws FileNotFoundException; }''',
"android/content/Context.java":'''package android.content;
import java.io.File;
public abstract class Context {
 public static final int MODE_PRIVATE=0;
 public abstract Context getApplicationContext(); public abstract File getFilesDir();
 public abstract ContentResolver getContentResolver();
 public abstract SharedPreferences getSharedPreferences(String name,int mode);
 public abstract Intent startForegroundService(Intent intent);
}''',
"android/content/Intent.java":'''package android.content;
public class Intent {
 public static final int FLAG_ACTIVITY_REORDER_TO_FRONT=0x00020000;
 private String className=""; private int flags;
 public Intent(){} public Intent(Context c,Class<?> cls){className=cls.getName();}
 public Intent setClassName(Context c,String name){className=name;return this;}
 public Intent addFlags(int value){flags|=value;return this;}
 public Intent putExtra(String key,String value){return this;}
 public String getClassName(){return className;} public int getFlags(){return flags;}
}''',
"android/net/Uri.java":'''package android.net;
import java.io.File;
public final class Uri {
 private final String scheme,id; private final boolean tree;
 private Uri(String scheme,String id,boolean tree){this.scheme=scheme;this.id=id;this.tree=tree;}
 public static Uri tree(String id){return new Uri("content",id,true);}
 public static Uri content(String id,boolean tree){return new Uri("content",id,tree);}
 public static Uri file(String id){return new Uri("file",id,false);}
 public static Uri fromFile(File file){return new Uri("file",file.getAbsolutePath(),false);}
 public String getScheme(){return scheme;} public String getId(){return id;} public boolean isTree(){return tree;}
 public String getAuthority(){return "test.provider";}
 public String toString(){return scheme+"://"+id;}
}''',
"android/database/Cursor.java":'''package android.database;
public interface Cursor extends AutoCloseable {
 boolean moveToNext(); String getString(int column); boolean isNull(int column); long getLong(int column); void close();
}''',
"android/os/SystemClock.java":'''package android.os; public final class SystemClock { public static long elapsedRealtime(){return System.nanoTime()/1000000L;} }''',
"android/provider/DocumentsContract.java":'''package android.provider;
import android.net.Uri;
public final class DocumentsContract {
 public static final class Document { public static final String COLUMN_DOCUMENT_ID="id",COLUMN_DISPLAY_NAME="name",COLUMN_MIME_TYPE="mime",COLUMN_SIZE="size",MIME_TYPE_DIR="vnd.android.document/directory"; }
 public static boolean isTreeUri(Uri u){return u!=null&&u.isTree();}
 public static String getTreeDocumentId(Uri u){return u.getId();}
 public static Uri buildChildDocumentsUriUsingTree(Uri tree,String id){return Uri.content(id,false);}
 public static Uri buildDocumentUriUsingTree(Uri tree,String id){return Uri.content(id,false);}
}''',
"org/json/JSONObject.java":'''package org.json;
public class JSONObject {
 private final String text; public JSONObject(){text="";} public JSONObject(String s){text=s==null?"":s;}
 public int length(){return text.isEmpty()?0:1;}
 public String optString(String key){return optString(key,"");}
 public String optString(String key,String fallback){String needle="\\\""+key+"\\\"";int p=text.indexOf(needle);if(p<0)return fallback;p=text.indexOf(':',p+needle.length());if(p<0)return fallback;p++;while(p<text.length()&&Character.isWhitespace(text.charAt(p)))p++;if(p>=text.length()||text.charAt(p++)!='\\\"')return fallback;int e=text.indexOf('\\\"',p);return e<0?fallback:text.substring(p,e);}
 public boolean optBoolean(String key){return text.contains("\\\""+key+"\\\":true");}
 public long optLong(String key,int fallback){return fallback;} public int optInt(String key,int fallback){return fallback;}
}''',
"com/lasrecomp/android/SharedFiles.java":'''package com.lasrecomp.android;
import android.content.Context; import org.json.JSONObject; import java.io.*; import java.nio.charset.StandardCharsets; import java.nio.file.Files;
final class SharedFiles {
 static final String[] TITLES=TitlePolicy.TITLES;
 static String checked(String t){TitlePolicy.get(t);return t;}
 static String label(String t){return checked(t);}
 static File game(Context c,String t){return new File(c.getFilesDir(),"games/"+checked(t)+"/extracted");}
 static File progress(Context c,String t){return new File(c.getFilesDir(),"builds/"+checked(t)+"/progress.json");}
 static File cancel(Context c,String t){return new File(c.getFilesDir(),"builds/"+checked(t)+"/cancel.request");}
 static boolean extractedImported(Context c,String t){File g=game(c,t);return new File(g,"exefs/main").isFile()&&new File(g,"exefs/sdk").isFile()&&("las".equals(t)||(new File(g,"exefs/subsdk0").isFile()&&new File(g,"exefs/subsdk1").isFile()))&&(new File(g,"romfs.bin").isFile()||new File(g,"romfs").isDirectory());}
 static boolean imported(Context c,String t){return extractedImported(c,t)||NativePackage.selection(c,t)!=null;}
 static final java.util.Map<String,Boolean> READY=new java.util.HashMap<>();
 static boolean ready(Context c,String t){return READY.getOrDefault(t,false);}
 static JSONObject read(File f){try{return new JSONObject(new String(Files.readAllBytes(f.toPath()),StandardCharsets.UTF_8));}catch(Exception e){return new JSONObject();}}
 static void atomic(File f,String s)throws IOException{File p=f.getParentFile();if(!p.isDirectory()&&!p.mkdirs())throw new IOException("mkdir");Files.write(f.toPath(),s.getBytes(StandardCharsets.UTF_8));}
 static void session(Context c,String t)throws IOException{checked(t);if(!ready(c,t))throw new IOException("not ready");}
 static String buildStatus(JSONObject p){return "";} static String buildStatus(JSONObject p,JSONObject a,String t){return "";}
}''',
"com/lasrecomp/android/NativePackage.java":'''package com.lasrecomp.android;
import android.content.Context; import android.net.Uri; import java.io.IOException;
final class NativePackage {
 static volatile boolean running; static boolean isRunning(){return running;} static String status(){return "";}
 static final java.util.Map<String,NativePackageFiles.Selection> SELECTIONS=new java.util.HashMap<>();
 static int starts; static String lastTitle; static Uri lastGame,lastKeys;
 static NativePackageFiles.Selection selection(Context c,String t){return SELECTIONS.get(t);}
 static void start(Context c,String t,Uri game,Uri keys)throws IOException{starts++;lastTitle=t;lastGame=game;lastKeys=keys;}
}''',
"com/lasrecomp/android/RaitchiMetadata.java":'''package com.lasrecomp.android;
import android.content.Context; import android.net.Uri; import java.io.File; import java.io.IOException;
public final class RaitchiMetadata {
 static final class GameInfo {final String title,developer,programId,programIdHex,version,iconPath,adapterId,reason;final boolean compatible,tryable;
  GameInfo(String adapter,boolean supported,String why){this(adapter,supported,false,why);}
  GameInfo(String adapter,boolean supported,boolean canTry,String why){title="Test title";developer="Test publisher";programId="1";programIdHex="0000000000000001";version="1.0";iconPath="";adapterId=adapter;compatible=supported;tryable=canTry;reason=why;}}
 static GameInfo next=new GameInfo("las",true,"");static int inspections;
 static GameInfo inspect(Context c,Uri source)throws IOException{inspections++;return next;}
 static File keysFile(Context c){return new File(new File(c.getFilesDir(),"shell"),"prod.keys");}
}''',
"com/lasrecomp/android/SharedLauncherActivity.java":'''package com.lasrecomp.android;
import android.content.Context;
public final class SharedLauncherActivity {
 static volatile int gamePid,buildPid;
 static int gamePid(Context c){return gamePid;}
 static int processPid(Context c,String suffix){return ":game".equals(suffix)?gamePid:":build".equals(suffix)?buildPid:0;}
}''',
"com/lasrecomp/android/RaitchiUiDriverImport.java":'''package com.lasrecomp.android;
import android.content.Context; import android.net.Uri;
final class RaitchiUiDriverImport { static boolean running; static String status(){return "";} static boolean isRunning(){return running;} static boolean start(Context c,Uri u){return true;} }''',
"com/lasrecomp/android/GpuDrivers.java":'''package com.lasrecomp.android;
import android.content.Context; import android.net.Uri; import java.io.IOException;
final class GpuDrivers { static final class Driver {String name,version;} static boolean failInstall;static int installs;
 static Driver find(Context c){return null;} static boolean selected(Context c){return false;} static void select(Context c,boolean b){}
 static String install(Context c,Uri uri)throws IOException{installs++;if(failInstall)throw new IOException("fixture install failure");return "Installed test driver";}
}''',
"com/lasrecomp/android/SharedBuildService.java":'''package com.lasrecomp.android; public final class SharedBuildService {}''',
}

def main():
    global BUILD
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--out',type=Path,default=BUILD)
    BUILD=parser.parse_args().out.resolve()
    if BUILD.exists():shutil.rmtree(BUILD)
    src=BUILD/"stubs"; classes=BUILD/"classes"
    for rel,body in STUBS.items():
        path=src/rel;path.parent.mkdir(parents=True,exist_ok=True);path.write_text(body+"\n")
    test=ROOT/"tests/RaitchiShellBridgeTest.java"
    files=[src/rel for rel in STUBS]+[
        JAVA/"SettingsStore.java",JAVA/"TitlePolicy.java",JAVA/"NativePackageFiles.java",
        JAVA/"RaitchiUiPackageStaging.java",JAVA/"RaitchiShellBridge.java",test]
    classes.mkdir(parents=True)
    subprocess.run(["javac","-Xlint:all","-d",str(classes),*[str(p) for p in files]],check=True)
    result=subprocess.run(["java","-Draitchi.test.root="+str(BUILD/"data"),"-cp",str(classes),"com.lasrecomp.android.RaitchiShellBridgeTest"],check=True,text=True,capture_output=True)
    print(result.stdout.strip())
    receipt={"schema":1,"scope":"Actual RaitchiShellBridge, RaitchiUiPackageStaging, NativePackageFiles and SettingsStore compiled/executed on host. Android Context/SAF/Uri, process IDs, NativePackage, RaitchiMetadata, GpuDrivers, and SharedFiles are explicit stand-ins; metadata/driver failures and stored keys path are controllable. SharedFiles stand-in keeps the production private games/<title>/extracted path/validity contract. Assertions cover metadata compatibility/adapter gates, generic tryable gating, private-key routing, same-URI reuse, cross-title blocking, NCE option filtering, and driver busy/failure cleanup. Android compilation is separately covered by Gradle. No device/runtime execution.","output":result.stdout.strip(),"source_sha256":{str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in [test,Path(__file__),JAVA/"RaitchiShellBridge.java",JAVA/"RaitchiUiPackageStaging.java",JAVA/"NativePackageFiles.java",JAVA/"SettingsStore.java",JAVA/"TitlePolicy.java"]}}
    out=BUILD/"verification.json";out.write_text(json.dumps(receipt,indent=2)+"\n")
    print("Host bridge contract checks passed; production Android APIs were represented by controlled stand-ins.")

if __name__=="__main__":main()
