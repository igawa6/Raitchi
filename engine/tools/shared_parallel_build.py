#!/usr/bin/env python3
"""Run the frozen shared producer with one to four compilation workers.

Package this operational adapter outside the toolkit's file inventory. Generation
remains serial; only the existing compile_objects ThreadPoolExecutor width changes.
All remaining options are validated by shared_ondevice_build.py, including paths,
titles and timeouts. Its hashes and generation records remain unchanged; verified
APK-installation-only object reuse retains the complete original provenance.
Example: --producer KIT/shared_ondevice_build.py --jobs 4 [producer options]
"""
from __future__ import annotations
import argparse
from contextlib import contextmanager
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import signal
import sys

PRODUCER_SHA256 = "f7163dbd66573e9bb2ee45ed2bbb82469515abebbf1c14824405f03b4c1815e5"


def load_producer(path):
    path = path.resolve()
    if hashlib.sha256(path.read_bytes()).hexdigest() != PRODUCER_SHA256:
        raise RuntimeError("Parallel adapter requires the verified frozen producer")
    spec = importlib.util.spec_from_file_location("shared_frozen_producer", path)
    module = importlib.util.module_from_spec(spec)
    # Compile exactly the verified bytes; do not load a stale adjacent pyc file.
    source = path.read_bytes()
    if hashlib.sha256(source).hexdigest() != PRODUCER_SHA256:
        raise RuntimeError("Producer changed during verification")
    exec(compile(source, str(path), "exec"), module.__dict__)
    return module


def installed_dir_spec(spec):
    """Only Android's randomized installation directory is non-semantic here.

    Explicit -nostdinc/sysroot/header flags and the entire remaining compiler and
    source specification still match exactly. Other version changes are misses.
    """
    compiler = spec.get("compiler", {})
    version = compiler.get("version", "")
    if (compiler.get("kind") != "zig" or not isinstance(version, str) or
            sum(line.startswith("InstalledDir:") for line in version.splitlines()) != 1):
        return None
    version, count = re.subn(r"(?m)^InstalledDir: /data/app/[^\r\n]+/lib/arm64$",
                             "InstalledDir: <verified-identical-android-compiler>", version)
    if count != 1:
        return None
    return {**spec, "compiler": {**compiler, "version": version}}


@contextmanager
def installed_dir_cache(producer, work):
    """Scoped, verified reuse adapter; never rewrite original object provenance."""
    original = producer.object_reusable
    candidates = {}
    for metadata in sorted((work / "objects").glob("*.json")):
        try:
            cached = json.loads(metadata.read_text())
            spec = cached["spec"]
            normalized = installed_dir_spec(spec)
            if normalized is not None and cached.get("key") == metadata.stem == producer.fingerprint(spec):
                candidates.setdefault(producer.fingerprint(normalized), []).append((metadata, cached))
        except (OSError, ValueError, KeyError, TypeError, AttributeError):
            continue

    def reusable(obj, metadata, spec):
        if original(obj, metadata, spec):
            return True
        normalized = installed_dir_spec(spec)
        if normalized is None:
            return False
        for previous, cached in candidates.get(producer.fingerprint(normalized), []):
            previous_object = previous.with_suffix(".o")
            if (installed_dir_spec(cached["spec"]) != normalized or
                    not original(previous_object, previous, cached["spec"])):
                continue
            # Preserve the original record and object. The current key describes
            # this invocation; provenance explicitly identifies the old compiler
            # installation that produced these verified, unchanged object bytes.
            producer.atomic_bytes(obj, previous_object.read_bytes())
            if producer.sha(obj) != cached["object_sha256"]:
                obj.unlink(missing_ok=True)
                continue
            producer.atomic_json(metadata, {
                "key": producer.fingerprint(spec), "spec": spec,
                "dependencies": cached["dependencies"], "object_sha256": cached["object_sha256"],
                "compatibility_reuse": {"rule": "android-zig-installeddir-only-v1",
                                        "original_metadata": cached},
            })
            return original(obj, metadata, spec)
        return False

    producer.object_reusable = reusable
    try:
        yield
    finally:
        producer.object_reusable = original


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--producer", required=True, type=Path)
    parser.add_argument("--jobs", type=int, choices=(1, 2, 3, 4), default=1)
    options, remaining = parser.parse_known_args(argv)
    producer = load_producer(options.producer)
    # The original parser still owns every validation rule. Worker count is an
    # execution policy, absent from all original generation/object identities.
    args = producer.parse_args([*remaining, "--jobs", str(min(options.jobs, 2))])
    if options.producer.resolve() != args.kit / "shared_ondevice_build.py":
        parser.error("Producer must be the frozen script in the selected toolkit")
    args.jobs = options.jobs
    runner = producer.Runner(args.cancel_file)
    progress = producer.Progress(args.progress, args.title)

    def cancel(signum, frame):
        runner.stop.set()

    previous = {s: signal.signal(s, cancel) for s in (signal.SIGINT, signal.SIGTERM)}
    locked = False
    try:
        with producer.build_lock(args.work), producer.build_lock(args.output), installed_dir_cache(producer, args.work):
            locked = True
            producer.execute(args, runner, progress)
        return 0
    except producer.Cancelled as error:
        progress.update(state="cancelled", message=str(error))
        return 130
    except (producer.BuildError, OSError, ValueError, KeyError, RuntimeError) as error:
        if locked:
            progress.update(state="failed", message=str(error))
        print(f"Shared parallel producer: {error}", file=sys.stderr)
        return 1
    finally:
        runner.close()
        for sig, handler in previous.items():
            signal.signal(sig, handler)


if __name__ == "__main__":
    raise SystemExit(main())
