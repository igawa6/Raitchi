#!/usr/bin/env python3
"""Validate production settings persistence/merging with explicit Android preference stand-ins."""
import argparse
import hashlib
import json
import subprocess
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
BUILD=ROOT/"build/raitchi-settings"
JAVA=ROOT/"ports/shared-android/apk/app/src/main/java/com/lasrecomp/android"

def main():
    global BUILD
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--out',type=Path,default=BUILD)
    BUILD=parser.parse_args().out.resolve()
    BUILD.mkdir(parents=True,exist_ok=True)
    stub=BUILD/"stubs/android/content"
    stub.mkdir(parents=True,exist_ok=True)
    (stub/"Context.java").write_text('package android.content; public abstract class Context { public static final int MODE_PRIVATE=0; public abstract SharedPreferences getSharedPreferences(String name,int mode); public abstract java.io.File getFilesDir(); }\n')
    (stub/"SharedPreferences.java").write_text('package android.content; public interface SharedPreferences { String getString(String key,String fallback); boolean contains(String key); Editor edit(); interface Editor { Editor putString(String key,String value); Editor remove(String key); boolean commit(); } }\n')
    classes=BUILD/"classes"
    classes.mkdir(exist_ok=True)
    source=JAVA/"SettingsStore.java"
    test=ROOT/"tests/RaitchiSettingsTest.java"
    json_stub=BUILD/"stubs/org/json/JSONObject.java";json_stub.parent.mkdir(parents=True,exist_ok=True)
    json_stub.write_text('package org.json; public class JSONObject { public JSONObject(){} public JSONObject(String s){} public int length(){return 0;} public String optString(String k){return "";} public String optString(String k,String v){return v;} public long optLong(String k,int v){return v;} public int optInt(String k,int v){return v;} public boolean optBoolean(String k){return false;} }\n')
    gpu_stub=BUILD/"stubs/com/lasrecomp/android/GpuDrivers.java";gpu_stub.parent.mkdir(parents=True,exist_ok=True)
    gpu_stub.write_text('package com.lasrecomp.android; final class GpuDrivers { static boolean selected(android.content.Context c){return true;} static void deleteTree(java.io.File f){java.io.File[] children=f.listFiles();if(children!=null)for(java.io.File child:children)deleteTree(child);f.delete();} }\n')
    package_stub=BUILD/"stubs/com/lasrecomp/android/NativePackage.java"
    package_stub.write_text('package com.lasrecomp.android; final class NativePackage {static NativePackageFiles.Selection selection(android.content.Context c,String t){try{return NativePackageFiles.current(c.getFilesDir(),t);}catch(java.io.IOException e){return null;}} static void verifyDocument(android.content.Context c,NativePackageFiles.Selection s){} }\n')
    subprocess.run(["javac","-d",str(classes),str(stub/"Context.java"),str(stub/"SharedPreferences.java"),str(json_stub),str(gpu_stub),str(package_stub),str(JAVA/"TitlePolicy.java"),str(JAVA/"NativePackageFiles.java"),str(source),str(JAVA/"SharedFiles.java"),str(test)],check=True)
    result=subprocess.run(["java","-Draitchi.test.root="+str(BUILD),"-cp",str(classes),"com.lasrecomp.android.RaitchiSettingsTest"],check=True,text=True,capture_output=True)
    print(result.stdout.strip())
    # Compile the actual UI/session Java against Android APIs, separately from the host stand-ins.
    android=Path.home()/"android-sdk/platforms/android-36/android.jar"
    if not android.is_file():raise RuntimeError("Android 36 SDK required for settings UI compile")
    target=BUILD/"android-classes";target.mkdir(exist_ok=True)
    driver=ROOT/"sources/las-android/android/app/src/main/java/com/lasrecomp/android/GpuDrivers.java"
    inputs=[JAVA/name for name in ["TitlePolicy.java","SettingsStore.java","SettingsActivity.java","SharedFiles.java","RaitchiUi.java","RaitchiUiDriverImport.java","NativePackage.java","NativePackageFiles.java"]]+[driver]
    subprocess.run(["javac","-cp",str(android),"-d",str(target),*[str(p) for p in inputs]],check=True)
    receipt={"schema":1,"scope":"Actual SettingsStore/SharedFiles with controlled SharedPreferences, JSONObject and driver-selection stand-ins; real temporary filesystem session/save tests; actual SettingsActivity/SharedFiles Android API compilation. No device or runtime graphics validation.","output":result.stdout.strip(),"source_sha256":{str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in [source,test,Path(__file__),*inputs]}}
    (BUILD/"verification.json").write_text(json.dumps(receipt,indent=2)+"\n")
    print("Android settings UI/session compilation passed")

if __name__=="__main__":main()
