#!/usr/bin/env python3
"""Asset-free continuous-NCE patch planner, emitter and instruction-byte checks."""
import argparse
import ctypes
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def machine_stub_check(library, destination):
    import unicorn as uc
    from unicorn import arm64_const as regs

    class Site(ctypes.Structure):
        _fields_ = [('original_pc', ctypes.c_uint64), ('resume_pc', ctypes.c_uint64),
                    ('original_word', ctypes.c_uint32), ('kind', ctypes.c_uint32),
                    ('operand', ctypes.c_uint32), ('original_kind', ctypes.c_uint32),
                    ('payload', ctypes.c_uint64)]

    lib = ctypes.CDLL(str(library))
    emit = lib.cnce_patch_emit
    emit.argtypes = [ctypes.POINTER(Site), ctypes.c_uint64, ctypes.c_uint64,
                     ctypes.c_uint64, ctypes.POINTER(ctypes.c_uint32),
                     ctypes.c_void_p, ctypes.c_size_t]
    resume = lib.cnce_resume_emit
    resume.argtypes = [ctypes.c_uint64, ctypes.c_uint64, ctypes.c_void_p, ctypes.c_size_t]
    site = Site(0x10000000, 0x10000004, 0xd4000061, 1, 3, 1, 0)
    body = ctypes.create_string_buffer(32)
    replacement = ctypes.c_uint32()
    assert emit(ctypes.byref(site), 0x10010000, 0x10020000, 0x10030000,
                ctypes.byref(replacement), body, len(body)) == 0
    mu = uc.Uc(uc.UC_ARCH_ARM64, uc.UC_MODE_ARM)
    mu.mem_map(0x10000000, 0x40000)
    mu.mem_map(0x20000000, 0x20000)
    mu.mem_write(site.original_pc, struct.pack('<I', replacement.value))
    mu.mem_write(0x10010000, body.raw)
    gprs = [getattr(regs, f'UC_ARM64_REG_X{i}') for i in range(31)]
    vectors = [getattr(regs, f'UC_ARM64_REG_Q{i}') for i in range(32)]
    for i, reg in enumerate(gprs):
        mu.reg_write(reg, 0x1234567800000000 + i)
    for i, reg in enumerate(vectors):
        mu.reg_write(reg, (0x9988776655443322 << 64) | i)
    state_regs = gprs + vectors + [regs.UC_ARM64_REG_NZCV,
                                  regs.UC_ARM64_REG_FPCR, regs.UC_ARM64_REG_FPSR]
    mu.reg_write(regs.UC_ARM64_REG_NZCV, 0xa0000000)
    mu.reg_write(regs.UC_ARM64_REG_FPCR, 0x01400000)
    mu.reg_write(regs.UC_ARM64_REG_FPSR, 0x10)
    mu.reg_write(regs.UC_ARM64_REG_SP, 0x20010000)
    before = {reg: mu.reg_read(reg) for reg in state_regs}
    mu.emu_start(site.original_pc, 0x10030000, count=5)
    assert mu.reg_read(regs.UC_ARM64_REG_PC) == 0x10030000
    assert mu.reg_read(regs.UC_ARM64_REG_SP) == 0x2000fff0
    assert struct.unpack('<QQ', mu.mem_read(0x2000fff0, 16)) == (before[gprs[16]], before[gprs[17]])
    assert mu.reg_read(gprs[16]) == 0x10020000
    assert mu.reg_read(gprs[17]) == 0x10030000
    assert all(mu.reg_read(reg) == value for reg, value in before.items()
               if reg not in (gprs[16], gprs[17]))
    assert resume(0x10010100, site.resume_pc, body, len(body)) == 0
    mu.mem_write(0x10010100, body.raw[:8])
    # Model only the gateway's agreed entry into the resume stub: saved pair is
    # still on guestSP-16, while scratch registers contain arbitrary values.
    mu.reg_write(gprs[16], 0x10010100)
    mu.reg_write(gprs[17], 0xaaaa)
    mu.emu_start(0x10010100, site.resume_pc, count=2)
    assert mu.reg_read(regs.UC_ARM64_REG_PC) == site.resume_pc
    assert mu.reg_read(regs.UC_ARM64_REG_SP) == 0x20010000
    assert all(mu.reg_read(reg) == value for reg, value in before.items())
    report = {'passed': True, 'implementation': 'actual emitted bytes in Unicorn',
              'unicorn': uc.__version__, 'exit_instructions': 5,
              'resume_instructions': 2, 'gpr_count': 31, 'vector_count': 32,
              'guest_x18_preserved': True, 'nzcv_fpcr_fpsr_preserved': True,
              'scope': 'exit/resume stubs only; host context assembly and hardware execution are separate tests'}
    destination.write_text(json.dumps(report, indent=2) + '\n')
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ndk', type=Path,
                        default=Path.home() / 'android-sdk/ndk/27.2.12479018')
    parser.add_argument('--unicorn-site-packages',type=Path,
                        help='Existing host Unicorn Python package directory; no download')
    args = parser.parse_args()
    if args.unicorn_site_packages:
        sys.path.insert(0,str(args.unicorn_site_packages.resolve()))
    out = ROOT / 'build/continuous-nce-patch'
    out.mkdir(parents=True, exist_ok=True)
    cc = shutil.which('clang')
    assert cc, 'clang required'
    tools = args.ndk / 'toolchains/llvm/prebuilt/linux-x86_64/bin'
    android = tools / 'aarch64-linux-android30-clang'
    source = ROOT / 'native/continuous-nce/patch_plan.c'
    header = source.with_suffix('.h')
    test = ROOT / 'tests/continuous_nce_patch_test.c'
    paths = [source, header, test, Path(__file__).resolve()]
    pinned = {str(p.relative_to(ROOT)): sha(p) for p in paths}

    def run(command, log):
        result = subprocess.run(list(map(str, command)), text=True,
                                capture_output=True, timeout=60)
        (out / log).write_text(result.stdout + result.stderr)
        if result.returncode:
            raise RuntimeError(log + ': ' + result.stdout + result.stderr)
        return result.stdout

    instructions = []
    exclusive_cases = []
    for width, pair in [(1, False), (2, False), (4, False), (8, False),
                        (4, True), (8, True)]:
        reg = 'x' if width == 8 else 'w'
        suffix = 'b' if width == 1 else 'h' if width == 2 else ''
        if pair:
            variants = [(f'ldxp {reg}0,{reg}3,[x2]', f'ldaxp {reg}0,{reg}3,[x2]'),
                        (f'stxp w1,{reg}0,{reg}3,[x2]', f'stlxp w1,{reg}0,{reg}3,[x2]')]
        else:
            variants = [(f'ldxr{suffix} {reg}0,[x2]', f'ldaxr{suffix} {reg}0,[x2]'),
                        (f'stxr{suffix} w1,{reg}0,[x2]', f'stlxr{suffix} w1,{reg}0,[x2]')]
        for relaxed, ordered in variants:
            exclusive_cases.append(len(instructions))
            instructions += [relaxed, ordered]
    casp_start = len(instructions)
    casp_expected = []
    for size, reg in enumerate(('w', 'x')):
        for order, mnemonic in enumerate(('casp', 'caspa', 'caspl', 'caspal')):
            for rs in range(0, 32, 2):
                for rt in range(0, 32, 2):
                    for rn in (0, 31):
                        regname = lambda n: reg + (str(n) if n < 31 else 'zr')
                        base = 'x0' if rn == 0 else 'sp'
                        instructions.append(f'{mnemonic} {regname(rs)},{regname(rs+1)},{regname(rt)},{regname(rt+1)},[{base}]')
                        casp_expected.append(0x08207c00 | size << 30 | (order & 1) << 22 |
                                             (order >> 1) << 15 | rs << 16 | rn << 5 | rt)
    casp_end = len(instructions)
    ordinary_start = len(instructions)
    instructions += ['nop', 'add x18,x19,x20', 'sub sp,sp,#16', 'b .', 'bl .',
                     'br x18', 'blr x19', 'ret', 'cbz x0,.', 'cbnz w1,.',
                     'tbz x2,#40,.', 'tbnz w3,#17,.', 'b.eq .', 'adr x0,.',
                     'adrp x1,.', 'ldr x0,[x18]', 'str x19,[sp]', 'ldar x0,[x2]',
                     'stlr x0,[x2]', 'clrex', 'dmb ish', 'dsb ish', 'isb',
                     'mrs x0,fpcr', 'msr fpcr,x0', 'mrs x0,fpsr', 'msr fpsr,x0',
                     'mrs x0,nzcv', 'msr nzcv,x0', 'mrs x0,ctr_el0']
    ordinary_end = len(instructions)
    exit_start = len(instructions)
    instructions += ['stp x16,x17,[sp,#-16]!', 'ldr x16,1f', 'ldr x17,2f',
                     'br x17', '1: .xword 0', '2: .xword 0',
                     'ldp x16,x17,[sp],#16']
    assembly = out / 'fixture.S'
    assembly.write_text('.arch armv8.1-a+lse\n.text\n' + '\n'.join(instructions) + '\n')
    run([android, '-c', assembly, '-o', out / 'fixture.o'], 'assemble.log')
    run([tools / 'llvm-objcopy', '--only-section=.text', '-O', 'binary',
         out / 'fixture.o', out / 'fixture.bin'], 'extract.log')
    raw = (out / 'fixture.bin').read_bytes()
    words = struct.unpack('<' + 'I' * (len(raw) // 4), raw)
    assert list(words[casp_start:casp_end]) == casp_expected, 'NDK CASP encoding disagrees with architecture fields'
    disassembly = run([tools / 'llvm-objdump', '-d', '--mattr=+lse', out / 'fixture.o'], 'disassembly.log')
    for mnemonic in ('casp', 'caspa', 'caspl', 'caspal'):
        assert disassembly.count('\t' + mnemonic + '\t') == len(casp_expected) // 4
    text = ['/* Generated independently by the NDK assembler; synthetic ISA. */',
            'typedef struct ExclusiveCase {uint32_t relaxed,ordered;} ExclusiveCase;',
            f'#define CNCE_EXCLUSIVE_CASE_COUNT {len(exclusive_cases)}',
            'static const ExclusiveCase exclusive_cases[]={']
    text += [f'{{0x{words[i]:08x}u,0x{words[i+1]:08x}u}},' for i in exclusive_cases]
    text += ['};', f'#define CNCE_CASP_CASE_COUNT {len(casp_expected)}', 'static const uint32_t casp_words[]={']
    text += [f'0x{x:08x}u,' for x in words[casp_start:casp_end]]
    text += ['};', f'#define CNCE_ORDINARY_COUNT {ordinary_end-ordinary_start}',
             'static const uint32_t ordinary_words[]={']
    text += [f'0x{x:08x}u,' for x in words[ordinary_start:ordinary_end]]
    text += ['};', 'static const uint32_t exit_stub_words[]={']
    text += [f'0x{x:08x}u,' for x in words[exit_start:exit_start+4]]
    text += ['};', f'static const uint32_t resume_first_word=0x{words[exit_start+8]:08x}u;']
    (out / 'continuous_nce_words.h').write_text('\n'.join(text) + '\n')
    common = ['-std=c11', '-Wall', '-Wextra', '-Werror', '-I', ROOT / 'native', '-I', out]
    reports = {}
    for mode, flags in [('optimized', ['-O2']),
                        ('sanitized', ['-O1', '-g', '-fsanitize=address,undefined',
                                       '-fno-omit-frame-pointer'])]:
        executable = out / mode
        run([cc, *common, *flags, source, test, '-o', executable], mode + '-build.log')
        report = json.loads(run([executable], mode + '-run.log'))
        assert report['state'] == 'passed' and report['checks'] > 170000
        reports[mode] = report
    library = out / 'libplanner.so'
    run([cc, *common, '-O2', '-shared', '-fPIC', source, '-o', library], 'host-library.log')
    stub_report = machine_stub_check(library, out / 'machine-stub.json')
    target = out / 'continuous-nce-patch-test'
    run([android, *common, '-O2', '-ffixed-x18', source, test, '-Wl,--no-undefined',
         '-Wl,-z,max-page-size=16384', '-o', target], 'android-link.log')
    assert pinned == {str(p.relative_to(ROOT)): sha(p) for p in paths}, 'Source changed during verification'
    report = {'schema': 1, 'host_tests': reports, 'emitted_stub_execution': stub_report,
              'lse_casp_encoding_roundtrip': {'passed': True, 'assembler_disassembler_cases': len(casp_expected),
                  'element_widths': [32, 64], 'all_even_first_register_pairs': True, 'base_examples': ['x0', 'sp'],
                  'hardware_atomic_execution_tested': False,
                  'primary_isa_reference': 'https://support.arm.com/documentation/ddi0602/2026-06/Base-Instructions/CASP--CASPA--CASPAL--CASPL--Compare-and-swap-pair-of-words-or-doublewords-in-memory-'},
              'android_cross_link_passed': True, 'hardware_execution': 'separate context probe pending',
              'source_sha256': pinned, 'android_test_sha256': sha(target),
              'scope': 'asset-free metadata and exact native stub bytes; no game/device execution or runtime mapping'}
    (out / 'verification.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
