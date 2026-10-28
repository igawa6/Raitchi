#!/usr/bin/env python3
"""Build or run an asset-free offscreen Vulkan address-predicate fixture.

Host: --build [--output build/shared-gpu-addresses]
Android (fresh app-UID helper process): --run /private/path/probe.so
  --driver-dir /private/gpu_driver/current/ --driver-lib vulkan.ad07xx.so
  --hooks-dir <application nativeLibraryDir> [--temp-dir <private dir>]
Run with the app's installed liblaspython.so and its normal LASPY_HOME/PATH.
No ADB, downloads, app actions, runtime exports, or guest code are used here.
"""
import argparse
import ctypes
import hashlib
import json
import re
from pathlib import Path
import shutil
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]
PREFIX = '''#version 450
layout(local_size_x=64) in;
layout(std430,set=0,binding=0) buffer Data { uvec4 v[]; } d;
layout(push_constant) uniform P { uint n; uint mode; } p;
'''
BODY = '''void main(){uint i=gl_GlobalInvocationID.x;if(i>=p.n)return;
uvec4 a=d.v[i];uint al=a.x,ah=a.y;
%s
uint ga=lo-0x40000000u;
bool oldok=hi-0x12u-uint(lo<0x40000000u)==0u && ga<0xd0000000u;
bool lexok=(hi>0x12u || (hi==0x12u && lo>=0x40000000u)) &&
           (hi<0x13u || (hi==0x13u && lo<0x10000000u));
d.v[p.n+i]=uvec4(uint(oldok),uint(lexok),ga,0xffffffffu);}
'''
SOURCES = {
    'address_fixed': PREFIX + BODY % 'uint lo=al,hi=ah;',
    'address_dynamic': PREFIX + BODY % 'uint lo=al+a.z;uint hi=ah+(int(a.z)<0?0xffffffffu:0u)+uint(lo<al);',
    'address_int64': '#version 450\n#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require\n'+PREFIX.split('\n',1)[1]+'''
void main(){uint i=gl_GlobalInvocationID.x;if(i>=p.n)return;uvec4 a=d.v[i];
uint64_t address=(uint64_t(a.y)<<32)|uint64_t(a.x);
uint64_t delta=address+(p.mode==0u?uint64_t(0):uint64_t(int64_t(int(a.z))))-uint64_t(0x1240000000ul);
d.v[p.n+i].w=uint(delta<uint64_t(0xd0000000ul));}
''',
}


def call(command):
    p=subprocess.run([str(x) for x in command],text=True,capture_output=True)
    if p.returncode:raise RuntimeError(p.stdout+p.stderr)
    return p.stdout+p.stderr


def sha(path):
    h=hashlib.sha256()
    with path.open('rb') as f:
        for data in iter(lambda:f.read(1024*1024),b''):h.update(data)
    return h.hexdigest()


def build(args):
    out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
    glslang=shutil.which('glslangValidator');validator=shutil.which('spirv-val')
    if not glslang or not validator:raise RuntimeError('Cached glslangValidator/spirv-val required; nothing downloaded')
    generated=[]
    for name,source in SOURCES.items():
        path=out/(name+'.comp');spv=path.with_suffix('.spv');path.write_text(source)
        call([glslang,'-V','--target-env','vulkan1.1',path,'-o',spv])
        call([validator,'--target-env','vulkan1.1',spv])
        data=spv.read_bytes();words=struct.unpack('<'+'I'*(len(data)//4),data)
        generated.append('static const uint32_t '+name+'[]={'+','.join(hex(x)+'u' for x in words)+'};\n')
    (out/'gpu_address_shaders.h').write_text(''.join(generated))
    base=ROOT/'work/las-baseline/build/android'
    toolchain=args.ndk/'toolchains/llvm/prebuilt/linux-x86_64/bin'
    adreno=base/'cxx/RelWithDebInfo/435q175j/arm64-v8a/_deps'
    include=base/'deps/include';volk=include/'volk/volk.c'
    archive=adreno/'adrenotools-build/libadrenotools.a'
    lib=out/'shared_gpu_address_probe.so'
    exports=out/'exports.map'
    exports.write_text('{ global: shared_gpu_addresses_probe; local: *; };\n')
    prebuilt=toolchain.parent
    cxx=prebuilt/'sysroot/usr/lib/aarch64-linux-android'
    unwind=prebuilt/'lib/clang/18/lib/linux/aarch64/libunwind.a'
    command=[toolchain/'aarch64-linux-android30-clang','-std=c11','-shared','-fPIC','-O1',
             '-fvisibility=hidden','-DVK_NO_PROTOTYPES','-Wl,--no-undefined','-Wl,-z,max-page-size=16384',
             '-Wl,--version-script='+str(exports),'-Wl,--exclude-libs,ALL',
             '-I'+str(out),'-I'+str(include),'-I'+str(include/'volk'),
             '-I'+str(adreno/'adrenotools-src/include'),ROOT/'tests/shared_gpu_address_probe.c',
             volk,archive,adreno/'adrenotools-build/lib/linkernsbypass/liblinkernsbypass.a',
             '-Wl,--start-group',cxx/'libc++_static.a',cxx/'libc++abi.a',unwind,'-Wl,--end-group',
             '-landroid','-ldl','-llog','-latomic','-o',lib]
    call(command)
    elf=call([toolchain/'llvm-readelf','--file-header','--dynamic','--dyn-syms','--wide',lib])
    if 'libc++_shared.so' in elf:raise RuntimeError('Probe unexpectedly requires shared C++ runtime')
    exported=[]
    for line in elf.splitlines():
        parts=line.split()
        if len(parts)>=8 and re.fullmatch(r'\d+:',parts[0]) and parts[4] in ('GLOBAL','WEAK') and parts[5]=='DEFAULT' and parts[6]!='UND':exported.append((parts[7],parts[3]))
    if exported!=[('shared_gpu_addresses_probe','FUNC')]:raise RuntimeError('Probe exported unexpected symbols: '+str(exported))
    if 'AArch64' not in elf or 'shared_gpu_addresses_probe' not in elf:raise RuntimeError('Probe target/export check failed')
    report={'probe':str(lib),'sha256':sha(lib),'scope':'cross-linked only; no device execution',
            'shaders':{n:sha(out/(n+'.spv')) for n in SOURCES},'exports':exported}
    (out/'build.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))


def execute(args):
    if args.driver_dir and (not args.driver_lib or not args.hooks_dir):raise RuntimeError('Custom driver needs --driver-lib and --hooks-dir')
    library=ctypes.CDLL(str(args.run.resolve()))
    fn=library.shared_gpu_addresses_probe
    fn.argtypes=[ctypes.c_char_p]*4+[ctypes.c_char_p,ctypes.c_size_t];fn.restype=ctypes.c_int
    buf=ctypes.create_string_buffer(4096)
    # adrenotools concatenates directory + filename verbatim.
    directory=(str(args.driver_dir).rstrip('/')+'/') if args.driver_dir else None
    values=[str(x).encode() if x else None for x in (directory,args.driver_lib,args.hooks_dir,args.temp_dir)]
    code=fn(*values,buf,len(buf));report=json.loads(buf.value.decode())
    report['probe_returncode']=code;report['scope']='synthetic offscreen compute; no game/runtime behavior proof'
    print(json.dumps(report,indent=2))
    if args.report:args.report.write_text(json.dumps(report,indent=2)+'\n')
    return code


def main():
    p=argparse.ArgumentParser(description=__doc__)
    mode=p.add_mutually_exclusive_group(required=True);mode.add_argument('--build',action='store_true');mode.add_argument('--run',type=Path)
    p.add_argument('--output',type=Path,default=ROOT/'build/shared-gpu-addresses')
    p.add_argument('--ndk',type=Path,default=ROOT.parent/'android-sdk/ndk/27.2.12479018')
    for name in ('driver-dir','hooks-dir','temp-dir','report'):p.add_argument('--'+name,type=Path)
    p.add_argument('--driver-lib')
    args=p.parse_args()
    if args.build:build(args);return 0
    return execute(args)


if __name__=='__main__':raise SystemExit(main())
