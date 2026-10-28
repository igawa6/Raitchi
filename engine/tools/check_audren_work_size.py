#!/usr/bin/env python3
"""Build/run portable work-size arithmetic against asset-free original-SDK fixtures."""
import hashlib,json,pathlib,shutil,subprocess
ROOT=pathlib.Path(__file__).resolve().parents[1];OUT=ROOT/'build/audren-work-size';SERVICE=ROOT/'ports/shared-android/runtime/services'
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def run(args):
 p=subprocess.run(list(map(str,args)),capture_output=True,text=True,timeout=60)
 if p.returncode:raise RuntimeError(p.stdout+p.stderr)
 return p.stdout.strip()
def main():
 OUT.mkdir(parents=True,exist_ok=True);fixtures_path=ROOT/'tests/fixtures/audren_work_sizes.json';data=json.loads(fixtures_path.read_text());rows=[];accepted=0
 assert len(data['fixtures'])>=40
 for f in data['fixtures']:
  raw=bytes.fromhex(f['raw52_hex']);assert len(raw)==52
  accepted+=f['sdk_manager_result']==0
  if f['sdk_manager_result']==0:
   assert f['sdk_manager_size']==f['sdk_server_formula_size']
   expected=f['sdk_server_formula_size'];status='AUDREN_WORK_SIZE_OK'
  else:
   assert f['sdk_manager_result']==0xc0299;expected=0;status='AUDREN_WORK_SIZE_REVISION'
  assert 0<=expected<2**63
  rows.append('{'+json.dumps(f['name'])+',{'+','.join(str(b) for b in raw)+'},UINT64_C('+str(expected)+'),'+status+'}')
 (OUT/'audren_work_size_fixtures.inc').write_text('static const Fixture fixtures[]={\n'+',\n'.join(rows)+'\n};\n')
 audio=SERVICE/'audio.c';text=audio.read_text();start=text.index('    case 1: { // GetWorkBufferSize',text.index('SERVICE("audren:u"'))
 end=text.index('    case 2:',start)
 begin_summary=text.index('// A bounded snapshot of the fixed audren:u request bytes');end_summary=text.index('SERVICE("audren:u"',begin_summary)
 (OUT/'audren_work_service.inc').write_text(text[begin_summary:end_summary]+'\nstatic void work_size_service(IpcReq *req,IpcResp *resp) { switch(req->cmd) {\n'+text[start:end]+' default: break; } }\n')
 ipc=SERVICE.parent/'ipc.h';shutil.copyfile(ipc,OUT/'ipc.h');(OUT/'kernel.h').write_text('#pragma once\n#include <stdint.h>\n#include <stddef.h>\ntypedef struct { int type; } KObject;\n')
 paths=[audio,ipc,SERVICE/'audren_work_size.c' ,SERVICE/'audren_work_size.h',SERVICE/'LICENSE-audren-work-size.txt',fixtures_path,ROOT/'tests/audren_work_size_test.c',pathlib.Path(__file__)];pins={str(p.relative_to(ROOT)):sha(p) for p in paths};results={}
 for variant,flags in [('optimized',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'])]:
  binary=OUT/variant;run([shutil.which('clang'),'-std=c11','-Wall','-Wextra','-Werror',*flags,'-I',OUT,'-I',SERVICE.parent,ROOT/'tests/audren_work_size_test.c',SERVICE/'audren_work_size.c','-o',binary]);results[variant]=run([binary])
 assert pins=={str(p.relative_to(ROOT)):sha(p) for p in paths},'Inputs changed during checks'
 report={'state':'passed','results':results,'oracle_fixture_count':len(rows),'oracle_manager_accepted':accepted,'source_sha256':pins,'scope':'Actual pure parser/formula versus independently recorded owned SDK manager/direct arithmetic outputs acrossREV1..REV11; synthetic configurations only revision-manager-accepted, not full renderer-valid or gameplay claims. Canary/unaligned/truncation/bad-tag/signed-count/overflow tests; no device, hostC++sizeof or audio allocation.'};(OUT/'verification.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
if __name__=='__main__':main()
