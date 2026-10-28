#!/usr/bin/env python3
"""Apply or check the declared user-visible text substitutions for pinned base files.

The pinned upstream base carries its own product name in user-visible strings.
ports/shared-android/branding-substitutions.json lists exact replacements with
expected counts. They apply only inside C/C++/Objective-C++ string literals, so
comments and copyright notices stay byte-identical, and only to base files that
have no canonical overlay. A count mismatch (for example after an upstream
update) fails instead of silently skipping.
"""

import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RULES = ROOT / "ports/shared-android/branding-substitutions.json"
TOKEN = re.compile(r'"(?:[^"\\\n]|\\.)*"|/\*.*?\*/|//[^\n]*', re.DOTALL)


def load():
    return json.loads(RULES.read_text(encoding="utf-8"))


def substitute(text, old, new):
    count = 0

    def repl(m):
        nonlocal count
        tok = m.group(0)
        if not tok.startswith('"'):
            return tok
        n = tok.count(old)
        count += n
        return tok.replace(old, new) if n else tok

    return TOKEN.sub(repl, text), count


def apply(base_runtime, overlay_runtime=None, write=True):
    """Apply rules to files under base_runtime; returns a receipt dict."""
    data = load()
    receipt = []
    for entry in data["files"]:
        rel = entry["path"]
        if overlay_runtime is not None and (overlay_runtime / rel).exists():
            raise RuntimeError(f"{rel} has a canonical overlay; edit the overlay instead")
        path = base_runtime / rel
        text = path.read_text(encoding="utf-8")
        for rule in entry["rules"]:
            text, count = substitute(text, rule["old"], rule["new"])
            if count != rule["count"]:
                raise RuntimeError(f"{rel}: expected {rule['count']} of {rule['old']!r}, found {count}")
            receipt.append({"path": rel, "old": rule["old"], "count": count})
        if write:
            path.write_text(text, encoding="utf-8")
    return receipt


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--base", type=Path, default=ROOT / "sources/las-android/runtime",
                    help="pinned base runtime directory (checked read-only)")
    args = ap.parse_args()
    receipt = apply(args.base, ROOT / "ports/shared-android/runtime", write=False)
    print(f"branding substitutions: {len(receipt)} rule(s) match the pinned base")
    return 0


if __name__ == "__main__":
    sys.exit(main())
