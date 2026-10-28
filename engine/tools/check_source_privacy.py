#!/usr/bin/env python3
"""Check prepared source for private paths, address literals and supplied private patterns."""
import argparse
import ipaddress
import json
from pathlib import Path
import re


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root',type=Path)
    parser.add_argument('--patterns',type=Path,help='Private literal patterns, one per line; not distributed')
    args=parser.parse_args()
    private=[s.casefold() for s in args.patterns.read_text().splitlines() if s.strip()] if args.patterns else []
    path_rule=re.compile(rb'/(?:home|mnt)/[A-Za-z0-9_-]+/')
    address_rule=re.compile(rb'(?<![\d.])(?:\d{1,3}\.){3}\d{1,3}(?![\d.])')
    failures=[];count=0
    for source in args.root.rglob('*'):
        if not source.is_file() or '.git' in source.relative_to(args.root).parts:continue
        count+=1;relative=str(source.relative_to(args.root));data=source.read_bytes()
        text=data.decode('utf-8',errors='ignore').casefold()
        kinds=[]
        if any(s in text or s in relative.casefold() for s in private):kinds.append('supplied private pattern')
        if path_rule.search(data):kinds.append('local path')
        for item in address_rule.finditer(data):
            try:ipaddress.IPv4Address(item[0].decode())
            except ValueError:continue
            kinds.append('address literal');break
        if kinds:failures.append({'file':relative,'categories':kinds})
    print(json.dumps({'scanned_files':count,'findings':failures},indent=2))
    raise SystemExit(bool(failures))


if __name__=='__main__':main()
