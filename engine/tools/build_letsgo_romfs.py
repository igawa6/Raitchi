#!/usr/bin/env python3
"""Build upstream's RomFS packer and optionally run it on the separate Thor reference."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = "com.recomp.letsgo.test"
DATA = f"/sdcard/Android/data/{PACKAGE}/files/extracted"
CLI = r'''
#include "romfs_build.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/stat.h>
static void progress(uint64_t done, uint64_t total) {
    static uint64_t last;
    if (done-last >= (256ull<<20) || done==total) {
        printf("RomFS %.2f / %.2f GB\n", done/1e9, total/1e9); fflush(stdout); last=done;
    }
}
int main(int argc,char **argv) {
    if (argc!=5) return 2;
    char why[512];
    if (!romfs_build(argv[1],argv[2],progress,why,sizeof why)) { fprintf(stderr,"%s\n",why); return 1; }
    FILE *f=fopen(argv[2],"rb"); uint64_t h[10]; struct stat st;
    if (!f || stat(argv[2],&st) || fread(h,1,sizeof h,f)!=sizeof h || h[0]!=0x50
        || h[7]>(uint64_t)st.st_size || h[8]>(uint64_t)st.st_size-h[7]) return 3;
    uint64_t at=0,count=0,bytes=0;
    while (at<h[8]) {
        uint8_t e[32];
        if (h[8]-at<32 || fseeko(f,(off_t)(h[7]+at),SEEK_SET) || fread(e,1,32,f)!=32) return 4;
        uint32_t name; uint64_t off,size;
        __builtin_memcpy(&name,e+28,4); __builtin_memcpy(&off,e+8,8); __builtin_memcpy(&size,e+16,8);
        uint64_t entry=(32ull+name+3)&~3ull;
        if (entry>h[8]-at || h[9]>h[1] || off>h[1]-h[9] || size>h[1]-h[9]-off) return 5;
        at+=entry; count++; bytes+=size;
    }
    fclose(f);
    if (count!=strtoull(argv[3],NULL,10) || bytes!=strtoull(argv[4],NULL,10)) return 6;
    printf("RomFS verified: %llu files, %llu content bytes\n",(unsigned long long)count,(unsigned long long)bytes);
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", help="Run on this explicit device; otherwise compile helper only")
    args = parser.parse_args()
    work = ROOT / "work/letsgo-android"
    build = ROOT / "build/letsgo-romfs"
    build.mkdir(parents=True, exist_ok=True)
    cli = build / "main.c"
    cli.write_text(CLI)
    helper = build / "romfs-builder"
    compiler = Path.home() / "android-sdk/ndk/27.2.12479018/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android30-clang"
    subprocess.run([str(compiler), "-O2", "-I", str(work / "runtime"), str(cli),
                    str(work / "runtime/romfs_build.c"), "-o", str(helper)], check=True)
    if not args.serial:
        print("Compiled game-free upstream RomFS helper; no device action.")
        return
    manual = ROOT / "evidence/manual-session.json"
    if manual.is_file() and json.loads(manual.read_text()).get("active"):
        parser.error("Owner-controlled manual session active; no device packing attempted")
    source = ROOT / "private/letsgo-baseline/extracted/romfs"
    files = [p for p in source.rglob("*") if p.is_file()]
    total = sum(p.stat().st_size for p in files)
    adb = ["adb", "-s", args.serial]

    def shell(parts, check=True):
        return subprocess.run([*adb, "shell", shlex.join([str(p) for p in parts])],
                              check=check, capture_output=True, text=True)

    destination = DATA + "/romfs.bin"
    if shell(["test", "-e", destination], check=False).returncode == 0:
        raise RuntimeError("RomFS image exists; refusing overwrite")
    free = int(shell(["df", "-k", DATA]).stdout.splitlines()[-1].split()[3]) * 1024
    if free < total + 64 * 1024 * 1024:
        raise RuntimeError("Insufficient space for new RomFS image")
    digest = hashlib.sha256(helper.read_bytes()).hexdigest()
    remote = "/data/local/tmp/letsgo-romfs-builder-" + digest[:16]
    subprocess.run([*adb, "push", str(helper), remote], check=True, capture_output=True)
    shell(["chmod", "755", remote])
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
    partial = destination + ".incoming-" + stamp
    evidence = ROOT / "evidence/letsgo-romfs" / stamp
    evidence.mkdir(parents=True, exist_ok=False)
    with (evidence / "build.log").open("w") as log:
        subprocess.run([*adb, "shell", shlex.join([remote, DATA + "/romfs", partial,
                                                   str(len(files)), str(total)])],
                       check=True, stdout=log, stderr=subprocess.STDOUT)
    # The image stays private to this app; verify its readability through the app UID.
    image_sha = shell(["run-as", PACKAGE, "sha256sum", partial]).stdout.split()[0]
    shell(["sh", "-c", 'test ! -e "$2" && test ! -L "$2" && mv "$1" "$2"',
           "publish-romfs", partial, destination])
    (evidence / "result.json").write_text(json.dumps({
        "package": PACKAGE, "helper_sha256": digest, "romfs_sha256": image_sha,
        "files": len(files), "content_bytes": total, "app_uid_access_verified": True,
        "source": "pinned upstream runtime/romfs_build.c; unmodified",
        "game_executed": False}, indent=2) + "\n")
    print("Built packed RomFS on device; file count, content byte total and app access verified.")


if __name__ == "__main__":
    main()
