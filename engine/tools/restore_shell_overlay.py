#!/usr/bin/env python3
"""Restore a reviewed shell snapshot over its actual Eden Git history; never commits."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def safe(root, relative):
    item = Path(relative)
    if item.is_absolute() or '..' in item.parts or '.git' in item.parts:
        raise ValueError('Unsafe manifest path')
    return root / item


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--snapshot', required=True, type=Path)
    parser.add_argument('--manifest', required=True, type=Path)
    parser.add_argument('--destination', required=True, type=Path)
    parser.add_argument('--clone-source', help='Optional local Git cache instead of upstream URL')
    args = parser.parse_args()
    snapshot = args.snapshot.resolve(strict=True)
    manifest = json.loads(args.manifest.read_text())
    destination = args.destination.absolute()
    if destination.exists():
        parser.error('Destination must not exist; this tool only creates an isolated checkout')
    for row in manifest['files']:
        source = safe(snapshot, row['file'])
        if source.is_symlink() or not source.is_file() or digest(source) != row['sha256']:
            raise RuntimeError('Snapshot differs from manifest: ' + row['file'])
    for relative in manifest['deleted_upstream_files']:
        safe(destination, relative)
    def git(*words):
        return subprocess.run(['git', *map(str, words)], check=True, capture_output=True, text=True).stdout.strip()
    git('clone', '--no-checkout', '--no-hardlinks', args.clone_source or manifest['upstream_url'], destination)
    git('-C', destination, 'checkout', '--detach', manifest['upstream_commit'])
    git('-C', destination, 'remote', 'set-url', 'origin', manifest['upstream_url'])
    for relative in manifest['deleted_upstream_files']:
        target = safe(destination, relative)
        if target.is_file() or target.is_symlink():
            target.unlink()
    for row in manifest['files']:
        target = safe(destination, row['file'])
        if target.is_symlink():
            target.unlink()
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(safe(snapshot, row['file']), target)
    git('-C', destination, 'merge-base', '--is-ancestor', manifest['upstream_commit'], 'HEAD')
    for row in manifest['files']:
        if digest(safe(destination, row['file'])) != row['sha256']:
            raise RuntimeError('Restored file differs: ' + row['file'])
    print(json.dumps({'head':git('-C', destination, 'rev-parse', 'HEAD'),
                      'overlay_files_verified':len(manifest['files']),
                      'upstream_history_preserved':True, 'committed':False,
                      'build_tested':False, 'build_requires_allow_unpinned_shell':True}, indent=2))


if __name__ == '__main__':
    main()
