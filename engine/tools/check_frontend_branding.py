#!/usr/bin/env python3
"""Check that the Raitchi frontend shows no upstream app or recomp project names.

The Android shell is adapted from Eden, and the engine started from community
recomp projects. Their names belong in license/notice screens, not in the
ordinary user interface. This check inspects:

  --apk FILE      every string resource (all locales) actually packaged in a
                  built APK, via `aapt2 dump resources`; this is authoritative
                  because it reflects resource shrinking
  --native FILE   text-like strings (contain a space, >= 10 bytes) inside a
                  packaged native library or every lib/*/*.so of an APK;
                  catches in-game overlay/menu/crash text in the :game process
  --source DIR    user-visible literals under DIR: Kotlin/Java UI calls
                  (setText/Toast/title/text attributes and similar), layout
                  XML text attributes, and string literals containing a space
                  (>= 8 chars) in C/C++/Objective-C++ outside comments

Notice/credit resources required by the GPL and MIT notices are allowlisted in
tools/frontend_branding_allowlist.json. Internal identifiers (package names
such as org.yuzu.yuzu_emu or com.lasrecomp.android, JNI names, environment
variables, log tags) are not user visible and are not reported.

Exit status: 0 = clean, 1 = findings, 2 = usage/tool error.
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

# Case-sensitive on purpose: "eden" is an ordinary word in some locales
# (Turkish "devam eden"), while the product is always written "Eden".
FORBIDDEN = [
    ("eden", re.compile(r"\bEden\b|\bEDEN\b|eden-emu|edenemu", re.UNICODE)),
    ("yuzu", re.compile(r"yuzu", re.IGNORECASE)),
    ("citron/sudachi", re.compile(r"\bCitron\b|\bSudachi\b")),
    ("recomp project", re.compile(
        r"\b[A-Za-z0-9]*Recomp\b|\brecomp\b|lasrecomp|letsgo-recomp|"
        r"LASRecomp|EoWRecomp|LetsGoRecomp|WonderRecomp", re.UNICODE)),
]

UI_CALL = re.compile(
    r"(?:setText|setTitle|setMessage|setSubtitle|makeText|setContentTitle|"
    r"setContentText|setTicker|setPositiveButton|setNegativeButton|"
    r"Text\(|title\s*=|text\s*=|message\s*=|label\s*=)"
    r"[^\n\"]*\"((?:[^\"\\]|\\.)*)\"")
XML_TEXT = re.compile(
    r"android:(?:text|title|hint|contentDescription|summary|label)\s*=\s*\"([^\"@?][^\"]*)\"")


def load_allowlist(path):
    if not path or not path.exists():
        return {"resources": [], "resource_prefixes": [], "files": [], "literals": []}
    data = json.loads(path.read_text(encoding="utf-8"))
    data.setdefault("resources", [])
    data.setdefault("resource_prefixes", [])
    data.setdefault("files", [])
    data.setdefault("literals", [])
    return data


def match_names(text):
    return [name for name, rx in FORBIDDEN if rx.search(text)]


def find_aapt2():
    explicit = os.environ.get("AAPT2")
    if explicit:
        return explicit
    found = shutil.which("aapt2")
    if found:
        return found
    sdk = os.environ.get("ANDROID_HOME") or os.environ.get("ANDROID_SDK_ROOT")
    if sdk:
        tools = sorted(Path(sdk, "build-tools").glob("*/aapt2"))
        if tools:
            return str(tools[-1])
    return None


def check_apk(apk, allow):
    aapt2 = find_aapt2()
    if not aapt2:
        print("error: aapt2 not found (set AAPT2 or ANDROID_HOME)", file=sys.stderr)
        sys.exit(2)
    out = subprocess.run([aapt2, "dump", "resources", str(apk)],
                         capture_output=True, text=True, check=False)
    if out.returncode != 0:
        print(out.stderr, file=sys.stderr)
        sys.exit(2)
    findings = []
    current = None
    res_line = re.compile(r"^\s*resource 0x[0-9a-f]+ (\S+)")
    val_line = re.compile(r"^\s*\(([^)]*)\) \"(.*)\"\s*$")
    for line in out.stdout.splitlines():
        m = res_line.match(line)
        if m:
            current = m.group(1)
            continue
        m = val_line.match(line)
        if not m or not current or not current.startswith("string/"):
            continue
        name = current.split("/", 1)[1]
        if name in allow["resources"] or any(
                name.startswith(p) for p in allow["resource_prefixes"]):
            continue
        hits = match_names(m.group(2))
        if hits:
            findings.append({"where": f"{apk.name}:{current}",
                             "locale": m.group(1) or "default",
                             "names": hits, "text": m.group(2)[:160]})
    return findings


PROSE = re.compile(rb"[\x20-\x7e\xc0-\xf4\x80-\xbf]{12,}")


def native_blobs(path):
    if path.suffix == ".apk":
        import zipfile
        with zipfile.ZipFile(path) as z:
            for name in z.namelist():
                if name.startswith("lib/") and name.endswith(".so"):
                    yield f"{path.name}!{name}", z.read(name)
    else:
        yield path.name, path.read_bytes()


def check_native(path, allow):
    findings = []
    seen = set()
    for where, blob in native_blobs(path):
        for m in PROSE.finditer(blob):
            text = m.group(0).decode("utf-8", errors="replace")
            # Prose only: identifiers, symbols, paths and env names have no spaces.
            if " " not in text or len(text) < 10 or text in seen or text in allow["literals"]:
                continue
            hits = match_names(text)
            if hits:
                seen.add(text)
                findings.append({"where": where, "locale": "native",
                                 "names": hits, "text": text[:160]})
    return findings


C_SUFFIXES = (".c", ".cc", ".cpp", ".h", ".hpp", ".m", ".mm", ".inc")
C_LITERAL = re.compile(r'"((?:[^"\\\n]|\\.)*)"')
C_COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.DOTALL)


def strip_c_comments(text):
    # Replace comments with spaces of equal length so line numbers survive;
    # string literals are protected by matching them first.
    def repl(m):
        tok = m.group(0)
        return tok if tok.startswith('"') else re.sub(r"[^\n]", " ", tok)
    return re.sub(r'"(?:[^"\\\n]|\\.)*"|/\*.*?\*/|//[^\n]*', repl, text,
                  flags=re.DOTALL)


def check_c_source(path, rel, allow):
    findings = []
    text = strip_c_comments(path.read_text(encoding="utf-8", errors="replace"))
    for lineno, line in enumerate(text.splitlines(), 1):
        if line.lstrip().startswith("#include"):
            continue
        for m in C_LITERAL.finditer(line):
            lit = m.group(1)
            if " " not in lit or len(lit) < 8 or lit in allow["literals"]:
                continue
            hits = match_names(lit)
            if hits:
                findings.append({"where": f"{rel}:{lineno}", "locale": "-",
                                 "names": hits, "text": lit[:160]})
    return findings


def check_source(root, allow):
    findings = []
    allowed_files = [re.compile(p) for p in allow["files"]]
    for path in sorted(root.rglob("*")):
        if not path.is_file() or \
                path.suffix not in (".kt", ".java", ".xml") + C_SUFFIXES:
            continue
        rel = path.relative_to(root).as_posix()
        if any(rx.search(rel) for rx in allowed_files):
            continue
        if path.suffix in C_SUFFIXES:
            findings += check_c_source(path, rel, allow)
            continue
        if path.suffix == ".xml" and "/res/layout" not in "/" + rel and \
                "/res/menu" not in "/" + rel and "/res/navigation" not in "/" + rel:
            continue
        rx = XML_TEXT if path.suffix == ".xml" else UI_CALL
        text = path.read_text(encoding="utf-8", errors="replace")
        for lineno, line in enumerate(text.splitlines(), 1):
            for m in rx.finditer(line):
                hits = match_names(m.group(1))
                if hits:
                    findings.append({"where": f"{rel}:{lineno}", "locale": "-",
                                     "names": hits, "text": m.group(1)[:160]})
    return findings


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--apk", type=Path, action="append", default=[])
    ap.add_argument("--native", type=Path, action="append", default=[])
    ap.add_argument("--source", type=Path, action="append", default=[])
    ap.add_argument("--allowlist", type=Path,
                    default=Path(__file__).with_name("frontend_branding_allowlist.json"))
    ap.add_argument("--json", type=Path, help="write findings as JSON")
    args = ap.parse_args()
    if not args.apk and not args.source and not args.native:
        ap.error("give at least one --apk, --native or --source")
    allow = load_allowlist(args.allowlist)
    findings = []
    for apk in args.apk:
        findings += check_apk(apk, allow)
    for lib in args.native:
        findings += check_native(lib, allow)
    for src in args.source:
        findings += check_source(src, allow)
    for f in findings:
        print(f"{f['where']} [{f['locale']}] {','.join(f['names'])}: {f['text']}")
    keys = {f["where"] for f in findings}
    print(f"frontend branding: {len(findings)} finding(s) in {len(keys)} location(s)")
    if args.json:
        args.json.write_text(json.dumps(findings, indent=1, ensure_ascii=False) + "\n",
                             encoding="utf-8")
    return 1 if findings else 0


if __name__ == "__main__":
    sys.exit(main())
