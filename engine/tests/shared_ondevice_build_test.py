"""Asset-free producer tests. Real NDK object/link tests use a tiny test generator,
not game generation: fake NSO inputs and fake common-runtime exports are explicit.
No private game inputs, device, downloads, or guest execution are used.
Run: python3 -m unittest discover -s tests -p shared_ondevice_build_test.py -v
"""
import importlib.util
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import shared_ondevice_build as builder
import build_shared_gamelib as verified

NDK = Path(os.environ.get('ANDROID_NDK_HOME', ROOT.parent / 'android-sdk/ndk/27.2.12479018'))
CC = NDK / 'toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android30-clang'
PYTHON = ROOT / 'work/las-baseline/build/android/venv/bin/python'
ZIG = ROOT / 'build/shared-ondevice/work/build/ondevice/zig-host/zig'
CCKIT = ROOT / 'build/shared-ondevice/stage/cckit'

GENERATOR = r'''import os,struct,sys
from pathlib import Path
name,nso,base,dest=sys.argv[1:5];base=int(base,16);rest=sys.argv[5:]
assert '--simd' in rest and rest[rest.index('--simd')+1]=='asm'
shift=int(rest[rest.index('--shift')+1],0) if '--shift' in rest else 0
if shift: assert ('--hle' in rest)==(name=='sdk')
out=Path(dest);out.mkdir(parents=True)
a=[base,base+4]
(out/(name+'_000.c')).write_text('#include "cpu.h"\n'+''.join(
 'void f_%x(Cpu*c){c->x[0]++;if(c->x[1])rt_udf(c,0);}\n'%x for x in a))
(out/(name+'_table.c')).write_text('#include "cpu.h"\n'+''.join('void f_%x(Cpu*c);\n'%x for x in a)+
 'const uint64_t %s_fn_addrs[]={%s};\n'%(name,','.join('0x%xull'%(x-shift) for x in a))+
 'const GuestFn %s_fn_ptrs[]={%s};\n'%(name,','.join('f_%x'%x for x in a))+
 'const uint64_t %s_fn_count=2;\n'%name)
print(name+': progress 2/2 files 1',file=sys.stderr)
'''


def synthetic_nso(build_id):
    data = bytearray(0x300)
    data[:4] = b'NSO0'
    struct.pack_into('<III', data, 0x10, 0x100, 0, 0x40)
    struct.pack_into('<III', data, 0x20, 0x140, 0x100, 0x40)
    struct.pack_into('<III', data, 0x30, 0x180, 0x200, 0x40)
    data[0x40:0x60] = bytes.fromhex(build_id)
    return bytes(data)


class ProducerTests(unittest.TestCase):
    def test_exact_descriptor_and_sidecar_contract(self):
        for title, policy in builder.POLICIES.items():
            modules = {name: {'module_id': ident, 'canonical_base': base, 'build_id': '11'*32,
                              'sha256': '22'*32, 'mapped_size': 16384, 'text_offset': 0,
                              'text_size': 256, 'function_count': 2}
                       for name, ident, base in policy['modules']}
            self.assertEqual(builder.entry_source(modules, policy), verified.entry_source(modules, policy))
            self.assertEqual(builder.wire_manifest(modules, policy, '33'*32, '44'*32, '55'*32),
                             verified.wire_manifest(modules, policy, '33'*32, '44'*32, '55'*32))
            self.assertEqual(len(builder.wire_manifest(modules, policy, '33'*32, '44'*32, '55'*32)),
                             176 + 112*len(modules))

    def test_dependency_cache_rehashes_content(self):
        with tempfile.TemporaryDirectory() as name:
            root=Path(name);obj=root/'a.o';meta=root/'a.json';dep=root/'header.h'
            obj.write_bytes(b'object');dep.write_text('header')
            spec={'source':'sourcehash'}
            builder.atomic_json(meta, {'spec':spec,'key':builder.fingerprint(spec),
                'object_sha256':builder.sha(obj),'dependencies':{str(dep):builder.sha(dep)}})
            self.assertTrue(builder.object_reusable(obj,meta,spec))
            dep.write_text('changed')
            self.assertFalse(builder.object_reusable(obj,meta,spec))
            dep.write_text('header');obj.write_bytes(b'tampered')
            self.assertFalse(builder.object_reusable(obj,meta,spec))
            meta.write_text('{')
            self.assertFalse(builder.object_reusable(obj,meta,spec))

    def test_immutable_pair_and_cancel_preserve_current(self):
        with tempfile.TemporaryDirectory() as name:
            root=Path(name);out=root/'output';out.mkdir();library=root/'new.so';library.write_bytes(b'first')
            runner=builder.Runner()
            old=builder.publish(out,library,b'first sidecar',{'fixture':True},runner)
            self.assertEqual((out/'current').resolve(),old)
            library.write_bytes(b'second');runner.stop.set()
            with self.assertRaises(builder.Cancelled):builder.publish(out,library,b'second sidecar',{},runner)
            self.assertEqual((out/'current').resolve(),old)
            self.assertEqual((old/'liblasgame.so').read_bytes(),b'first')
            runner=builder.Runner();new=builder.publish(out,library,b'second sidecar',{},runner)
            self.assertNotEqual(old,new);self.assertTrue(old.is_dir())
            (new/'liblasgame.so.saotm').write_bytes(b'tampered')
            with self.assertRaises(builder.BuildError):builder.publish(out,library,b'second sidecar',{},runner)

    def test_cancel_terminates_active_helper(self):
        with tempfile.TemporaryDirectory() as name:
            root=Path(name);cancel=root/'cancel';runner=builder.Runner(cancel)
            timer=threading.Timer(0.3,lambda:cancel.write_text('cancel'))
            timer.start();start=time.monotonic()
            try:
                with self.assertRaises(builder.Cancelled):
                    runner.run([sys.executable,'-c','import time;time.sleep(30)'],root/'child.log')
            finally:timer.join();runner.close()
            self.assertLess(time.monotonic()-start,5)
            self.assertFalse(runner.children)

    def test_toolkit_path_rejects_traversal(self):
        with tempfile.TemporaryDirectory() as name:
            root=Path(name)
            for path in ('../outside','/absolute','a/../../b','bad\\path'):
                with self.assertRaises(builder.BuildError):builder.kit_path(root,path)
            (root/'escape').symlink_to(root.parent)
            with self.assertRaises(builder.BuildError):builder.kit_path(root,'escape/x')

    def test_elf_rejects_bad_target_and_truncation(self):
        with tempfile.TemporaryDirectory() as name:
            p=Path(name)/'bad.so'
            for content in (b'',b'not an elf',b'\0'*64):
                p.write_bytes(content)
                with self.assertRaises(builder.BuildError):builder.verify_elf(p)

    @unittest.skipUnless(CC.is_file() and PYTHON.is_file(), 'cached NDK and generator Python required')
    def test_real_aarch64_pipeline_two_and_four_modules_and_resume(self):
        cases=[(t,'clang',CC) for t in ('las','eevee')]
        if ZIG.is_file() and (CCKIT/'sysroot').is_dir():
            cases += [(t,'zig',ZIG) for t in ('las','eevee')]
        for title,kind,compiler in cases:
            with self.subTest(title=title,compiler=kind), tempfile.TemporaryDirectory(prefix='shared-producer-') as name:
                root=Path(name);kit=root/'kit';native=root/'native';native.mkdir();kit.mkdir()
                runtime=root/'runtime.c';runtime.write_text('void rt_udf(void*c,unsigned long long p){(void)c;(void)p;}\n')
                subprocess.run([str(CC),'-shared','-fPIC','-Wl,-soname,libmain.so',str(runtime),'-o',str(native/'libmain.so')],check=True,capture_output=True)
                if kind=='zig':
                    for dirname in ('clang','sysroot'):shutil.copytree(CCKIT/dirname,kit/'cckit'/dirname)
                for source,dest in ((ROOT/'native/runtime/aot_cpu.h',kit/'cckit/include/runtime/aot_cpu.h'),
                                    (ROOT/'native/module_abi.h',kit/'cckit/include/module_abi.h')):
                    dest.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(source,dest)
                expected_titles={};game=root/'game';(game/'exefs').mkdir(parents=True)
                for t,policy in builder.POLICIES.items():
                    src=kit/'generators'/t/'tools';src.mkdir(parents=True)
                    for f in ('gen.py','nso.py','lifter.py','analyze.py','patches.py'):
                        (src/f).write_text(GENERATOR if f=='gen.py' else '# synthetic fixture dependency\n')
                    hle=src.parent/'runtime/hle/hle_symbols.txt';hle.parent.mkdir(parents=True);hle.write_text('')
                    includes=kit/'cckit/include/profiles'/t;includes.mkdir(parents=True)
                    for f in ('simd.h','simd_fp.h'):shutil.copyfile(ROOT/'sources'/('las-android' if t=='las' else 'letsgo')/'runtime'/f,includes/f)
                    modules=[]
                    for module,ident,base in policy['modules']:
                        bid=(policy['main_prefix'] if module=='main' else '12')
                        bid=(bid+'ab'*32)[:64]
                        raw=synthetic_nso(bid);tmp=root/'input';tmp.write_bytes(raw)
                        modules.append({'name':module,'module_id':ident,'canonical_base':base,'build_id':bid,
                                        'sha256':builder.sha(tmp),'mapped_size':16384,'text_offset':0,'text_size':64})
                        if t==title:(game/'exefs'/module).write_bytes(raw)
                    expected_titles[t]={'source_commit':policy['commit'],'modules':modules}
                npdm=bytearray(0x98);npdm[:4]=b'META';struct.pack_into('<I',npdm,0x70,0x80)
                npdm[0x80:0x84]=b'ACI0';struct.pack_into('<Q',npdm,0x90,builder.POLICIES[title]['program_id'])
                (game/'exefs/main.npdm').write_bytes(npdm)
                if title=='las':
                    (game/'exefs/subsdk0').write_bytes(b'inert unselected original')
                    (game/'exefs/subsdk1').write_bytes(b'inert unselected original')
                contract=builder.hashlib.sha256(b'shared-aot-v1\0'+(kit/'cckit/include/runtime/aot_cpu.h').read_bytes()+(kit/'cckit/include/module_abi.h').read_bytes()).hexdigest()
                record={'schema':1,'contract_sha256':contract,'runtime_sha256':builder.sha(native/'libmain.so'),
                        'compiler_kind':kind,'files':{str(p.relative_to(kit)):builder.sha(p) for p in kit.rglob('*') if p.is_file()},
                        'native_files':{'libmain.so':builder.sha(native/'libmain.so')},'titles':expected_titles}
                builder.atomic_json(kit/'shared-build-kit.json',record)
                work=root/'work';output=root/'output'
                argv=['--title',title,'--game-root',str(game),'--work',str(work),'--output',str(output),
                      '--kit',str(kit),'--native-lib-dir',str(native),'--progress',str(work/'progress.json'),
                      '--cancel-file',str(work/'cancel'),'--jobs','2','--compiler',str(compiler),'--compiler-kind',kind,'--python',str(PYTHON)]
                rc=builder.main(argv)
                self.assertEqual(rc,0,"\n".join(p.read_text(errors="replace")[-3000:] for p in work.glob("*.log")))
                first=(output/'current').resolve();status=json.loads((work/'progress.json').read_text())
                (game/'exefs/subsdk2').write_bytes(b'unknown module')
                self.assertEqual(builder.main(argv),1)
                self.assertEqual((output/'current').resolve(),first)
                (game/'exefs/subsdk2').unlink()
                self.assertEqual(status['state'],'complete');self.assertEqual(status['compiled'],len(builder.POLICIES[title]['modules'])*2+1)
                manifest=(first/'liblasgame.so.saotm').read_bytes()
                self.assertEqual(manifest[:8],b'SAOTM001');self.assertEqual(manifest[144:176],bytes.fromhex(builder.sha(first/'liblasgame.so')))
                rc=builder.main(argv)
                self.assertEqual(rc,0,"\n".join(p.read_text(errors="replace")[-3000:] for p in work.glob("*.log")))
                status=json.loads((work/'progress.json').read_text());self.assertEqual(status['compiled'],0);self.assertEqual(status['reused'],status['total'])
                self.assertEqual((output/'current').resolve(),first)
                metadata=json.loads((first/'build.json').read_text());Path(metadata['objects'][0]['object']).write_bytes(b'corrupted object')
                rc=builder.main(argv)
                self.assertEqual(rc,0,"\n".join(p.read_text(errors="replace")[-3000:] for p in work.glob("*.log")))
                status=json.loads((work/'progress.json').read_text());self.assertEqual(status['compiled'],1)
                (game/'exefs/main').write_bytes(b'wrong title')
                self.assertEqual(builder.main(argv),1)
                self.assertEqual((output/'current').resolve(),first)
                self.assertEqual((first/'liblasgame.so.saotm').read_bytes(),manifest)


if __name__ == '__main__':
    unittest.main()
