#!/usr/bin/env python3
"""Host regression checks for real Raitchi UI lifecycle helpers, no device actions."""
import hashlib,json,re,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def main():
 out=ROOT/'build/raitchi-ui-lifecycle';out.mkdir(parents=True,exist_ok=True);stubs=out/'stubs'
 texts={
 'android/content/Context.java':'package android.content; public class Context {public Context application; public Context getApplicationContext(){return application==null?this:application;}}',
 'android/net/Uri.java':'package android.net; public class Uri {}',
 'com/lasrecomp/android/GpuDrivers.java':'''package com.lasrecomp.android;import android.content.Context;import android.net.Uri;import java.util.concurrent.*;import java.util.concurrent.atomic.*;final class GpuDrivers {static final CountDownLatch entered=new CountDownLatch(1),release=new CountDownLatch(1);static final AtomicInteger calls=new AtomicInteger();static volatile Context received;static String install(Context context,Uri uri)throws Exception{received=context;calls.incrementAndGet();entered.countDown();if(!release.await(5,TimeUnit.SECONDS))throw new Exception("test timeout");return "test driver";}}'''}
 for name,text in texts.items():p=stubs/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(text+'\n')
 app=ROOT/'ports/shared-android/apk/app/src/main/java/com/lasrecomp/android';sources=[app/'RaitchiUiDriverImport.java',app/'RaitchiUiPackageStaging.java',ROOT/'tests/raitchi_ui_lifecycle_test.java'];paths=sources+[app/'SettingsActivity.java',app/'SharedLauncherActivity.java',Path(__file__).resolve()];pins={str(p.relative_to(ROOT)):sha(p) for p in paths}
 def run(args,name):
  r=subprocess.run(list(map(str,args)),capture_output=True,text=True,timeout=30);(out/name).write_text(r.stdout+r.stderr)
  if r.returncode:raise RuntimeError(name+': '+r.stdout+r.stderr)
  return r.stdout
 classes=out/'classes';run(['javac','--release','17','-d',classes,*sources,*sorted(stubs.rglob('*.java'))],'host-compile.log');stdout=run(['java','-cp',classes,'com.lasrecomp.android.RaitchiUiLifecycleTest'],'host-run.log');checks=int(re.search(r'checks=(\d+)',stdout)[1])
 assert pins=={str(p.relative_to(ROOT)):sha(p) for p in paths},'Source changed during lifecycle tests'
 report={'schema':1,'state':'passed','checks':checks,'source_sha256':pins,'scope':'real atomic driver lifecycle/cleanup/publication helpers; stand-in Android installer/context, synthetic files and child-JVM interrupted imports; no actual driver install or Android lifecycle/device execution'};(out/'verification.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
if __name__=='__main__':main()
