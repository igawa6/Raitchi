#!/usr/bin/env python3
"""Run real native-package publication tests and compile actual Android launcher Java."""
import argparse,hashlib,json,re,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
JAVA=ROOT/'ports/shared-android/apk/app/src/main/java/com/lasrecomp/android'
def main():
 parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--out',type=Path,default=ROOT/'build/native-package-ui')
 out=parser.parse_args().out.resolve();out.mkdir(parents=True,exist_ok=True)
 def run(cmd,name):
  p=subprocess.run(list(map(str,cmd)),capture_output=True,text=True,timeout=60);(out/name).write_text(p.stdout+p.stderr)
  if p.returncode:raise RuntimeError(name+': '+p.stdout+p.stderr)
  return p.stdout
 source=JAVA/'NativePackageFiles.java';test=ROOT/'tests/NativePackageFilesTest.java'
 run(['javac','--release','17','-d',out/'host-classes',JAVA/'TitlePolicy.java',source,test],'host-build.log')
 result=run(['java','-cp',out/'host-classes','com.lasrecomp.android.NativePackageFilesTest',out],'host-run.log')
 # Existing installed build's classes supply unchanged SDL/LasActivity symbols;
 # every changed actual overlay is compiled from source against Android36.
 sdk=Path.home()/'android-sdk/platforms/android-36/android.jar'
 prior=ROOT/'build/shared-android/apk/app/intermediates/javac/debug/compileDebugJavaWithJavac/classes'
 sources=sorted(JAVA.glob('*.java'))
 run(['javac','-cp',str(sdk)+':'+str(prior),'-d',out/'android-classes',*sources],'android-build.log')
 paths=[*sources,test,Path(__file__).resolve()]
 report={'schema':1,'state':'passed','host_checks':int(re.search(r'checks=(\d+)',result)[1]),'actual_android36_java_compile':True,
 'scope':'Actual native-generation validation/atomic publication with synthetic files, previous-generation/save/code preservation and failure-path tests. Android compile of actual overlays; no real package/JNI/SAF/device qualification.',
 'source_sha256':{str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}}
 (out/'verification.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
if __name__=='__main__':main()
