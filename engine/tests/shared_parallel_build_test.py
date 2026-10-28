"""Asset-free operational adapter checks; no compiler, game, or device execution."""
import hashlib
from contextlib import nullcontext
import json
from pathlib import Path
import sys
import tempfile
import threading
import time
import types
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import shared_parallel_build as parallel


class ParallelTests(unittest.TestCase):
    def test_frozen_producer_required(self):
        path = ROOT / "tools/shared_ondevice_build.py"
        self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(), parallel.PRODUCER_SHA256)
        self.assertEqual(Path(parallel.load_producer(path).__file__), path)
        with tempfile.TemporaryDirectory() as directory:
            changed = Path(directory) / path.name
            changed.write_bytes(path.read_bytes() + b"\n# changed\n")
            with self.assertRaises(RuntimeError):
                parallel.load_producer(changed)

    def test_adapter_delegates_validation_workers_and_cancellation(self):
        producer = parallel.load_producer(ROOT / "tools/shared_ondevice_build.py")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            kit = root / "kit"
            kit.mkdir()
            script = kit / "shared_ondevice_build.py"
            script.write_bytes((ROOT / "tools/shared_ondevice_build.py").read_bytes())
            argv = ["--producer", str(script), "--jobs", "4", "--title", "eevee"]
            for name in ("game-root", "work", "output", "kit", "native-lib-dir"):
                argv += ["--" + name, str(root / name.replace("-root", "").replace("-dir", ""))]
            argv += ["--progress", str(root / "work/progress.json")]
            seen = []

            def execute(args, runner, progress):
                seen.append(args.jobs)
                self.assertEqual(args.kit, kit)
                raise producer.Cancelled("Build cancelled; completed objects remain resumable")

            with patch.object(parallel, "load_producer", return_value=producer), patch.object(producer, "execute", execute):
                for jobs in (1, 2, 3, 4):
                    self.assertEqual(parallel.main(argv + ["--jobs", str(jobs)]), 130)
                self.assertEqual(seen, [1, 2, 3, 4])
                self.assertEqual(json.loads((root / "work/progress.json").read_text())["state"], "cancelled")
                # The original producer still rejects overlapping mutable/input paths.
                invalid = argv + ["--output", str(kit / "bad-output")]
                with self.assertRaises(SystemExit):
                    parallel.main(invalid)
                self.assertEqual(seen, [1, 2, 3, 4])
            with self.assertRaises(SystemExit):
                parallel.main(argv + ["--jobs", "5"])

    def test_actual_producer_pool_four_and_cache_identity_unchanged(self):
        producer = parallel.load_producer(ROOT / "tools/shared_ondevice_build.py")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            kit, work, generated = root / "kit", root / "work", root / "generated"
            work.mkdir()
            for name in ("runtime/aot_cpu.h", "module_abi.h", "profiles/las/simd.h", "profiles/las/simd_fp.h"):
                path = kit / "cckit/include" / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("/* controlled header fixture */\n")
            generated.mkdir()
            hashes = {}
            for i in range(5):
                path = generated / ("unit%d.c" % i)
                path.write_text("/* controlled generated unit %d */\n" % i)
                hashes[path.name] = producer.sha(path)
            module = {"module_id": 1, "build_id": "11" * 32, "sha256": "22" * 32,
                      "canonical_base": 0x7000000000, "mapped_size": 16384,
                      "text_offset": 0, "text_size": 64, "function_count": 2}
            old_compiler = root / "old-install/libzig.so"
            old_compiler.parent.mkdir()
            old_compiler.write_bytes(b"identical compiler fixture")
            args = types.SimpleNamespace(work=work, kit=kit, title="las", jobs=2, unit_timeout=10,
                compiler_kind="zig", compiler=old_compiler, native_lib_dir=old_compiler.parent)
            progress = producer.Progress(work / "progress.json", "las")

            class CompilerFixture:
                """Observe real executor scheduling without executing a compiler."""
                def __init__(self):
                    self.lock = threading.Lock()
                    self.active = self.peak = self.compiled = 0
                    self.barrier = None
                    self.installed_dir = "/data/app/old/lib/arm64"
                    self.compiler_paths = []

                def check(self):
                    pass

                def close(self):
                    pass

                def run(self, argv, log, **kwargs):
                    if "--version" in argv:
                        self.compiler_paths.append(argv[0])
                        return ("fixture command\nclang version 22.1.8\nTarget: aarch64-unknown-linux-musl\n"
                                "Thread model: posix\nInstalledDir: " + self.installed_dir + "\n")
                    if "-MD" not in argv:
                        return ""
                    with self.lock:
                        self.active += 1
                        self.peak = max(self.peak, self.active)
                        self.compiled += 1
                    try:
                        if self.barrier:
                            self.barrier.wait(timeout=5)
                        time.sleep(0.02)
                        source = Path(argv[argv.index("-c") + 1])
                        output = Path(argv[argv.index("-o") + 1])
                        output.write_bytes(b"fixture object:" + source.read_bytes())
                        Path(argv[argv.index("-MF") + 1]).write_text("object: " + str(source) + "\n")
                    finally:
                        with self.lock:
                            self.active -= 1
                    return ""

            record = {"contract_sha256": "33" * 32, "native_files": {"libzig.so": producer.sha(old_compiler)},
                      "files": {str(p.relative_to(kit)): producer.sha(p) for p in kit.rglob("*") if p.is_file()}}
            runner = CompilerFixture()
            with nullcontext():
                first, code, _, semantic = producer.compile_objects(args, record, producer.POLICIES["las"],
                    {"main": module}, generated, hashes, "same-generation-request", runner, progress)
                self.assertEqual(runner.peak, 2)
                self.assertEqual(runner.compiled, 6)
                args.jobs = 4
                resumed, resumed_code, _, resumed_semantic = producer.compile_objects(args, record, producer.POLICIES["las"],
                    {"main": module}, generated, hashes, "same-generation-request", runner, progress)
                self.assertTrue(all(item["reused"] for item in resumed))
                self.assertEqual(code, resumed_code)
                self.assertEqual(semantic, resumed_semantic)
                for item in resumed[:4]:
                    Path(item["object"]).write_bytes(b"corruption")
                runner.peak = runner.compiled = 0
                runner.barrier = threading.Barrier(4)
                repaired, _, _, _ = producer.compile_objects(args, record, producer.POLICIES["las"],
                    {"main": module}, generated, hashes, "same-generation-request", runner, progress)
                self.assertEqual(runner.peak, 4)
                self.assertEqual(runner.compiled, 4)
                self.assertEqual(sum(item["reused"] for item in repaired), 2)
                self.assertEqual({item["key"] for item in first}, {item["key"] for item in repaired})
                # APK updates relocate identical tools. Exercise the ORIGINAL
                # compiler_tools --version parser, not a constant identity mock.
                original_metadata = {item["key"]: Path(item["object"]).with_suffix(".json").read_bytes() for item in repaired}
                new_compiler = root / "new-install/libzig.so"
                new_compiler.parent.mkdir()
                new_compiler.write_bytes(old_compiler.read_bytes())
                args.compiler = new_compiler
                args.native_lib_dir = new_compiler.parent
                runner.installed_dir = "/data/app/new/lib/arm64"
                runner.compiled = 0
                runner.barrier = None
                with parallel.installed_dir_cache(producer, work):
                    migrated, _, _, _ = producer.compile_objects(args, record, producer.POLICIES["las"],
                        {"main": module}, generated, hashes, "same-generation-request", runner, progress)
                self.assertEqual(runner.compiled, 0)
                self.assertTrue(all(item["reused"] for item in migrated))
                self.assertEqual(runner.compiler_paths[-1], str(new_compiler))
                self.assertNotEqual({item["key"] for item in first}, {item["key"] for item in migrated})
                for item in migrated:
                    metadata = json.loads(Path(item["object"]).with_suffix(".json").read_text())
                    origin = metadata["compatibility_reuse"]["original_metadata"]
                    self.assertEqual(origin, json.loads(original_metadata[origin["key"]]))
                    self.assertIn("InstalledDir: /data/app/new/lib/arm64", metadata["spec"]["compiler"]["version"])
                    self.assertIn("InstalledDir: /data/app/old/lib/arm64", origin["spec"]["compiler"]["version"])
                for key, content in original_metadata.items():
                    self.assertEqual((work / "objects" / (key + ".json")).read_bytes(), content)

    def test_installed_dir_reuse_rejects_all_other_changes(self):
        producer = parallel.load_producer(ROOT / "tools/shared_ondevice_build.py")
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            objects = work / "objects"
            objects.mkdir()
            dep = work / "header.h"
            dep.write_bytes(b"header")
            spec = {"generation": "generation", "unit": "unit.c", "source_sha256": "11" * 32,
                    "headers": {"header.h": producer.sha(dep)}, "flags": ["-O1"],
                    "compiler": {"kind": "zig", "sha256": "22" * 32, "native_files": {"libzig.so": "22" * 32},
                        "toolkit_sha256": "33" * 32,
                        "version": "clang version 22.1.8\nInstalledDir: /data/app/old/lib/arm64"}}
            key = producer.fingerprint(spec)
            obj, meta = objects / (key + ".o"), objects / (key + ".json")
            obj.write_bytes(b"original object")
            producer.atomic_json(meta, {"key": key, "spec": spec, "object_sha256": producer.sha(obj),
                "dependencies": {str(dep): producer.sha(dep)}})
            current = json.loads(json.dumps(spec))
            current["compiler"]["version"] = spec["compiler"]["version"].replace("/old/", "/new/")
            variants = []
            for field, value in (("generation", "changed"), ("source_sha256", "44" * 32),
                                 ("headers", {}), ("flags", ["-O0"])):
                variants.append({**current, field: value})
            for field, value in (("sha256", "55" * 32), ("native_files", {}), ("toolkit_sha256", "66" * 32),
                                 ("version", current["compiler"]["version"].replace("22.1.8", "22.1.9"))):
                variants.append({**current, "compiler": {**current["compiler"], field: value}})
            with parallel.installed_dir_cache(producer, work):
                for changed in variants:
                    target = producer.fingerprint(changed)
                    self.assertFalse(producer.object_reusable(objects / (target + ".o"), objects / (target + ".json"), changed))
                dep.write_bytes(b"changed header")
                target = producer.fingerprint(current)
                self.assertFalse(producer.object_reusable(objects / (target + ".o"), objects / (target + ".json"), current))
                dep.write_bytes(b"header")
                obj.write_bytes(b"corruption")
                self.assertFalse(producer.object_reusable(objects / (target + ".o"), objects / (target + ".json"), current))


if __name__ == "__main__":
    unittest.main()
