#!/usr/bin/env python3
"""Audit an original SDK Relocate entry and build exact AOT differentials.

Original bytes, addresses, generated body and producer records stay ignored.
No device actions, original SDK startup, game startup or runtime edits.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
from audit_game_inputs import load_parser
from audit_native_sdk_thunk import definitions
ROOT=Path(__file__).resolve().parents[1]
SYMBOL='_ZN2nn4util15RelocationTable8RelocateEv'
def sha(path):
 with Path(path).open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()
def need(value,message):
 if not value:raise RuntimeError(message)
def ignored(path):need(subprocess.run(['git','check-ignore','-q',str(path)],cwd=ROOT).returncode==0,'Private file must remain ignored: '+str(path))
def candidate(prepare_path,game_receipt_path,game_sha,out):
 import capstone
 ignored(game_receipt_path);prepare=json.loads(prepare_path.read_text());semantic=prepare['semantic'];need(hashlib.sha256(json.dumps(semantic,sort_keys=True).encode()).hexdigest()==prepare['semantic_code_sha256'],'Host semantic fingerprint changed')
 need(semantic['title']=='las' and semantic['cpu_identity']==3,'Expected reviewed LAS CPU3')
 sdk=semantic['modules']['sdk'];original=Path(sdk['path']);need(sha(original)==sdk['sha256'] and original.read_bytes()[0x40:0x60].hex()==sdk['build_id'].lower(),'Original SDK identity changed')
 Nso,backend,parser_path=load_parser();nso=Nso(original);matches=[s for s in nso.syms if s[0]==SYMBOL and s[2] and (s[1]&15)==2];need(len(matches)==1,'Original SDK export missing/ambiguous');symbol=matches[0];offset,size=symbol[3:5]
 need(size==368 and offset%4==0 and nso.text[0]<=offset and offset+size<=sum(nso.text),'Original symbol shape changed')
 need(not any(site<offset+size and site+8>offset for site,*_ in nso.relocs),'Relocation touches original body')
 code=bytes(nso.image[offset:offset+size]);canonical=sdk['canonical_base']+offset;actual=canonical-semantic['guest_shift'];name='f_'+format(canonical,'x');generated=Path(prepare['generated_directory']);table=generated/'sdk/sdk_table.c';need(sha(table)==semantic['generated_sha256']['sdk/sdk_table.c'],'Generated table changed')
 table_text=table.read_text();address_match=re.search(r'const uint64_t sdk_fn_addrs\[\]\s*=\s*\{(.*?)\};',table_text,re.S);function_match=re.search(r'const GuestFn sdk_fn_ptrs\[\]\s*=\s*\{(.*?)\};',table_text,re.S);need(address_match and function_match,'Generated table declaration changed')
 addresses=[int(x,16) for x in re.findall(r'0x([0-9a-fA-F]+)ull',address_match[1])];functions=re.findall(r'\bf_[0-9a-fA-F]+\b',function_match[1]);need(len(addresses)==len(functions)==sdk['function_count'] and actual in addresses,'Generated SDK table slot missing');slot=addresses.index(actual);need(functions[slot]==name,'Generated function pointer mismatch')
 definition=definitions(generated,[name],semantic['generated_sha256'])[name];body=definition['definition_excerpt'];need(not re.search(r'\b(?:rt_dispatch|rt_svc|rt_hook|rt_call_original|guest_call)\s*\(',body) and not re.findall(r'\bf_[0-9a-f]+\(c\)',body),'Candidate calls guest/runtime services')
 md=capstone.Cs(capstone.CS_ARCH_ARM64,capstone.CS_MODE_ARM);md.detail=True;instructions=list(md.disasm(code,canonical));need(sum(i.size for i in instructions)==size and instructions[-1].mnemonic=='ret','Incomplete original decode')
 for i in instructions:
  need(i.mnemonic not in ('bl','blr','svc','mrs','msr','adr','adrp'),'Original candidate requires external behavior')
  if i.group(capstone.CS_GRP_JUMP):
   targets=[op.imm for op in i.operands if op.type==capstone.arm64.ARM64_OP_IMM];need(len(targets)==1 and canonical<=targets[0]<canonical+size,'Branch escapes original body')
 receipt=json.loads(game_receipt_path.read_text());produced=receipt['semantic'];fingerprint=hashlib.sha256(json.dumps(produced,sort_keys=True).encode()).hexdigest();need(receipt['library_sha256']==game_sha and fingerprint==receipt['semantic_code_sha256'],'Actual game producer receipt rejected');need(receipt['title']==produced['title']=='las' and produced['cpu_identity']==3 and produced['guest_shift']==semantic['guest_shift'] and produced['contract']==semantic['contract'],'Actual producer ABI/layout differs')
 for key in ('module_id','sha256','build_id','canonical_base','mapped_size','text_offset','text_size','function_count'):need(produced['modules']['sdk'][key]==sdk[key],'Actual producer SDK differs: '+key)
 bound={}
 for path in (table,Path(definition['definition_unit'])):
  relative=str(path.relative_to(generated));digest=sha(path);need(digest==produced['generated_sha256'][relative]==semantic['generated_sha256'][relative],'Actual generated source differs from audited body/table')
  semantic_objects=[item for item in produced['objects'] if item['unit']==relative];linked_objects=[item for item in receipt['objects'] if item['unit']==relative];need(len(semantic_objects)==len(linked_objects)==1,'Actual object record missing/ambiguous');a,b=semantic_objects[0],linked_objects[0]
  for key in ('key','sha256'):need(a[key]==b[key] and re.fullmatch(r'[0-9a-f]{64}',a[key]) and a[key]!='0'*64,'Actual object fingerprint invalid')
  need(bool(b['object']),'Actual object path missing');bound[relative]={'source_sha256':digest,'object_key':a['key'],'object_sha256':a['sha256']}
 record={'schema':1,'state':'audited-not-device-executed','method':'pinned NSO parser and Capstone, not REA','symbol':SYMBOL,'original':{'sha256':sha(original),'full_build_id':sdk['build_id'],'code_sha256':hashlib.sha256(code).hexdigest(),'offset':offset,'size':size,'parser_sha256':sha(parser_path),'parser_backend':backend},'aot_address':actual,'aot_canonical_address':canonical,'aot_table_index':slot,'aot_function_name':name,'aot_prepare_sha256':sha(prepare_path),'aot_host_semantic_sha256':prepare['semantic_code_sha256'],'aot_definition':definition,'actual_game_producer':{'receipt_sha256':sha(game_receipt_path),'library_sha256':game_sha,'semantic_code_sha256':fingerprint,'sidecar_sha256':receipt['sidecar_sha256'],'audited_units':bound},'original_instructions':[{'address':i.address,'mnemonic':i.mnemonic,'operands':i.op_str} for i in instructions]}
 path=out/'candidate.json';ignored(path);path.write_text(json.dumps(record,indent=2)+'\n')
 reference=out/'reference.c';reference.write_text('#define AOT_PROFILE_CAPABILITIES 0\n#include "runtime/aot_cpu.h"\nvoid '+body+'\n');(out/'relocation_reference.h').write_text('#define RELOCATION_REFERENCE '+name+'\nvoid '+name+'(Cpu *);\n')
 return record,path

def main():
 parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--runtime-sha256',required=True);parser.add_argument('--game-sha256',required=True);parser.add_argument('--runtime-receipt',type=Path,default=ROOT/'build/shared-android/native-build.json');parser.add_argument('--game-receipt',type=Path,default=ROOT/'evidence/ondevice-full-las-20261025T051302Z/published-build.json');parser.add_argument('--prepare',type=Path,default=ROOT/'build/shared-gamelib/las/prepare.json');parser.add_argument('--ndk',type=Path,default=Path.home()/'android-sdk/ndk/27.2.12479018');args=parser.parse_args()
 for digest in (args.runtime_sha256,args.game_sha256):need(re.fullmatch(r'[0-9a-f]{64}',digest),'Full hashes required')
 runtime_receipt=json.loads(args.runtime_receipt.read_text());need(runtime_receipt['library_sha256']==args.runtime_sha256,'Runtime receipt differs');need(runtime_receipt['shared_contract_sha256']==json.loads(args.prepare.read_text())['semantic']['contract'],'Runtime contract differs');out=ROOT/'build/native-sdk-relocation-probe';out.mkdir(parents=True,exist_ok=True);record,candidate_path=candidate(args.prepare,args.game_receipt,args.game_sha256,out)
 source=ROOT/'ports/shared-android/runtime/native_sdk_fastpath.c';header=ROOT/'ports/shared-android/runtime/native_sdk_fastpath.h';probe=ROOT/'tests/native_sdk_relocation_probe.c';paths=[source,header,probe,ROOT/'native/runtime/aot_cpu.h',ROOT/'native/module_abi.h',ROOT/'tools/audit_game_inputs.py',ROOT/'tools/audit_native_sdk_thunk.py',Path(__file__).resolve()];pinned={str(p.relative_to(ROOT)):sha(p) for p in paths}
 def run(command,name):
  result=subprocess.run(list(map(str,command)),capture_output=True,text=True,timeout=60);(out/name).write_text(result.stdout+result.stderr)
  if result.returncode:raise RuntimeError(str(command[0])+' failed: '+result.stdout+result.stderr)
  return result.stdout
 cc=shutil.which('clang');need(cc,'Host clang required');includes=['-I',ROOT/'native','-I',ROOT/'ports/shared-android/runtime','-I',out];tests={}
 for name,flags in [('optimized',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'])]:
  executable=out/name;run([cc,'-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-label',*flags,'-DNATIVE_SDK_RELOCATION_STANDALONE','-DNATIVE_SDK_RELOCATION_HOST_TEST',*includes,source,probe,out/'reference.c','-o',executable],name+'-build.log');test=json.loads(run([executable],name+'-run.log'));need(test['state']=='passed' and test['failures']==0,'Host actual-AOT-body differential failed');tests[name]=test
 tools=args.ndk/'toolchains/llvm/prebuilt/linux-x86_64/bin';exports=out/'exports.map';exports.write_text('NATIVE_SDK_RELOCATION_PROBE_1 { global: native_sdk_relocation_probe; local: *; };\n');helper=out/'libnative_sdk_relocation_probe.so';run([tools/'aarch64-linux-android30-clang','-std=c11','-O2','-g','-Wall','-Wextra','-Werror','-ffixed-x18','-fPIC','-fvisibility=hidden','-shared','-DNATIVE_SDK_RELOCATION_STANDALONE',*includes,source,probe,'-ldl','-Wl,--no-undefined','-Wl,--version-script='+str(exports),'-Wl,-z,max-page-size=16384','-Wl,-z,relro','-Wl,-z,now','-Wl,-z,noexecstack','-o',helper],'android-build.log');symbols=run([tools/'llvm-nm','--dynamic','--defined-only',helper],'exports.log');need([line.split()[-1].split('@')[0] for line in symbols.splitlines()]==['native_sdk_relocation_probe'],'Unexpected exports')
 disassembly=run([tools/'llvm-objdump','--no-show-raw-insn','-d',helper],'disassembly.txt');bad=[]
 for line in disassembly.splitlines():
  match=re.match(r'\s*[0-9a-f]+:\s+([a-z0-9.]+)\s+(.*)',line)
  if not match:continue
  op,operands=(part.lower() for part in match.groups())
  if ((op=='msr' and re.match(r'tpidr(?:ro)?_el0\b',operands)) or (re.match(r'[xw]18(?:,|$)',operands) and op not in ('str','stur','stlr','stp','stnp')) or (op in ('ldp','ldnp') and re.match(r'[^,]+,\s*[xw]18\b',operands))):bad.append((op,operands))
 need(not bad,'Helper writes physical X18/TLS: '+repr(bad))
 config={'schema':1,'runtime_sha256':args.runtime_sha256,'game_sha256':args.game_sha256,'helper_sha256':sha(helper),'candidate_sha256':sha(candidate_path),'original_sdk_sha256':record['original']['sha256'],'sidecar_sha256':record['actual_game_producer']['sidecar_sha256'],'aot_address':record['aot_address'],'actual_game_producer':record['actual_game_producer'],'outer_timeout_seconds':60};(out/'config.json').write_text(json.dumps(config,indent=2)+'\n');runner=out/'run_probe.py';runner.write_text('''import ctypes,hashlib,json,os,sys
config=json.load(open(sys.argv[1]));runtime,helper,root,game,sidecar=sys.argv[2:7]
for path,key in ((runtime,'runtime_sha256'),(helper,'helper_sha256'),(game,'game_sha256'),(sidecar,'sidecar_sha256'),(os.path.join(root,'exefs','sdk'),'original_sdk_sha256')):
 with open(path,'rb') as f: digest=hashlib.file_digest(f,'sha256').hexdigest()
 if digest!=config[key]: raise SystemExit('Hash mismatch: '+key)
for key in ('LARECOMP_CPU_BACKEND','LARECOMP_NATIVE_SDK_RELOCATE_FASTPATH'):os.environ.pop(key,None)
lib=ctypes.CDLL(helper);probe=lib.native_sdk_relocation_probe;probe.argtypes=[ctypes.c_char_p]*4+[ctypes.c_uint64,ctypes.c_void_p,ctypes.c_size_t];probe.restype=ctypes.c_int
output=ctypes.create_string_buffer(4096);status=probe(*map(os.fsencode,(runtime,root,game,sidecar)),config['aot_address'],output,len(output));report=json.loads(output.value);report.update({k:config[k] for k in ('runtime_sha256','game_sha256','helper_sha256','candidate_sha256')});print(json.dumps(report),flush=True);raise SystemExit(status!=0)
''');need(pinned=={str(p.relative_to(ROOT)):sha(p) for p in paths},'Sources changed during verification');verification={'schema':1,'host_tests':tests,'host_reference':'independently verified actual producer function body, compiled unchanged with profiling disabled; not Android descriptor execution','android_cross_link_passed':True,'device_execution':'pending actual verified title descriptor','physical_x18_tls_writes':bad,'candidate_sha256':sha(candidate_path),'runtime_receipt_sha256':sha(args.runtime_receipt),'actual_game_producer':record['actual_game_producer'],'sources_sha256':pinned,'artifacts_sha256':{str(p.relative_to(ROOT)):sha(p) for p in (helper,out/'config.json',runner,candidate_path,out/'reference.c',out/'relocation_reference.h')},'scope':'exact same-address resource byte differential including flags/nonidempotent second calls; actual title descriptor validation pending Thor'};(out/'verification.json').write_text(json.dumps(verification,indent=2)+'\n');print(json.dumps({'host_tests':tests,'helper_sha256':sha(helper),'android_cross_link_passed':True},indent=2))
if __name__=='__main__':main()
