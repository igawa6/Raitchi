#!/usr/bin/env python3
"""Validate compatibility reports and render the compatibility table.

Uses only the standard library so it runs in any CI job. The checks mirror
compatibility/schema.json; keep both in sync.
"""

import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
STATES = ["untested", "fails-startup", "boots", "renders", "interactive",
          "playable-tested-section", "completed"]
SECTIONS = {
    "game": {"title", "program_id", "version", "content"},
    "build": {"app_version", "runtime_sha256", "source_commit"},
    "device": {"model", "soc", "gpu_driver", "android"},
    "test": {"date", "tester", "route", "observation"},
    "result": {"state", "limitations"},
}
OPTIONAL = {"build": {"apk_sha256"}, "device": {"settings"},
            "test": {"fps", "session_seconds", "save"}, "result": {"issues"}}
SHA = re.compile(r"^[0-9a-f]{64}$")
# Reports are public: refuse local paths, addresses and serial-like tokens.
PRIVATE = re.compile(r"/(?:home|mnt|root)/|[A-Za-z]:\\\\|\b(?:192\.168|10\.\d+|100\.\d+)\.\d+\.\d+\b"
                     r"|\bserial\b", re.IGNORECASE)


def validate(path):
    errors = []
    try:
        rec = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        return None, [f"unreadable: {exc}"]
    if rec.get("schema") != 1:
        errors.append("schema must be 1")
    extra = set(rec) - set(SECTIONS) - {"schema"}
    if extra:
        errors.append(f"unknown top-level fields {sorted(extra)}")
    for sec, req in SECTIONS.items():
        val = rec.get(sec)
        if not isinstance(val, dict):
            errors.append(f"missing section {sec}")
            continue
        missing = req - set(val)
        unknown = set(val) - req - OPTIONAL.get(sec, set())
        if missing:
            errors.append(f"{sec}: missing {sorted(missing)}")
        if unknown:
            errors.append(f"{sec}: unknown {sorted(unknown)}")
    if errors:
        return rec, errors
    g, b, t, r = rec["game"], rec["build"], rec["test"], rec["result"]
    if not re.fullmatch(r"0100[0-9A-F]{12}", g["program_id"]):
        errors.append("game.program_id must be 16 upper-case hex digits starting 0100")
    if g["content"] not in ("base", "base+update", "base+dlc"):
        errors.append("game.content invalid")
    for key in ("runtime_sha256", "apk_sha256"):
        if b.get(key) is not None and not SHA.match(b[key]):
            errors.append(f"build.{key} must be a lower-case SHA-256")
    if not re.fullmatch(r"\d{4}-\d{2}-\d{2}", t["date"]):
        errors.append("test.date must be YYYY-MM-DD")
    if t["observation"] not in ("observed", "carried-over"):
        errors.append("test.observation must be observed or carried-over")
    if t["observation"] == "observed" and not b["runtime_sha256"]:
        errors.append("an observed result needs build.runtime_sha256")
    if len(t["route"]) < 10:
        errors.append("test.route must describe what was tested")
    if r["state"] not in STATES:
        errors.append(f"result.state must be one of {STATES}")
    if not isinstance(r["limitations"], list):
        errors.append("result.limitations must be a list")
    elif r["state"] in ("interactive", "playable-tested-section", "completed") \
            and not r["limitations"]:
        errors.append("list the known limitations (or 'none observed in route')")
    text = json.dumps(rec, ensure_ascii=False)
    if PRIVATE.search(text):
        errors.append("contains a private path, LAN address or serial reference")
    if re.search(r"\d+\s*%\s*(compatib|complete|playable)", text, re.IGNORECASE):
        errors.append("do not express compatibility as a percentage")
    return rec, errors


def table(records):
    rows = ["| Game | Version | State | Tested | Build (runtime) | Route | Limitations |",
            "|---|---|---|---|---|---|---|"]
    for rec in sorted(records, key=lambda x: x["game"]["title"]):
        g, b, t, r = rec["game"], rec["build"], rec["test"], rec["result"]
        state = r["state"] + (" (stale)" if t["observation"] == "carried-over" else "")
        rt = (b["runtime_sha256"] or "unknown")[:12]
        lim = "; ".join(r["limitations"]).replace("|", "/")
        rows.append(f"| {g['title']} | {g['version']} {g['content']} | {state} | "
                    f"{t['date']} {rec['device']['model']} | {rt} | {t['route']} | {lim} |")
    return "\n".join(rows) + "\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--reports", type=Path, default=ROOT / "compatibility" / "reports")
    ap.add_argument("--table", action="store_true", help="print the Markdown table")
    args = ap.parse_args()
    records, failed = [], False
    files = sorted(args.reports.glob("*.json"))
    if not files:
        print(f"no reports in {args.reports}", file=sys.stderr)
        return 1
    for path in files:
        rec, errors = validate(path)
        for e in errors:
            print(f"{path.name}: {e}", file=sys.stderr)
        failed |= bool(errors)
        if rec and not errors:
            records.append(rec)
    if args.table:
        sys.stdout.write(table(records))
    else:
        print(f"compatibility: {len(files)} report(s), {'FAIL' if failed else 'ok'}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
