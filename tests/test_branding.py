#!/usr/bin/env python3
"""AC-NativeNX branding and attribution checks (no build, no device, no game data).

Run: python3 -m unittest discover -s tests -p 'test_branding.py' -v
"""
import hashlib
import json
from pathlib import Path
import re
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]
APP_ID = "com.accreation.nativenx"
OLD_APP_ID = "dev.igawa6.raitchi"
APP_NAME = "AC-NativeNX"
# GPLv3 text exactly as shipped by igawa6/Raitchi v0.0.2 (root COPYING and engine/COPYING).
UPSTREAM_COPYING_SHA256 = "b6a8bf63ae2c95b0aa256ee774680d7bd18f21f457da75466f7610b3b16acc4e"
# Files that configure or verify the build (history/notes in *.md may mention the old id).
BUILD_SUFFIXES = {".kts", ".gradle", ".py", ".json", ".xml", ".properties", ".yml", ".yaml", ".sh", ".txt"}


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def tracked_files():
    out = subprocess.run(["git", "-C", str(ROOT), "ls-files", "-z"], check=True,
                         capture_output=True).stdout.decode()
    return [ROOT / name for name in out.split("\0") if name]


class BrandingTest(unittest.TestCase):
    def test_no_old_application_id_in_build_files(self):
        offenders = []
        for path in tracked_files():
            if path.suffix not in BUILD_SUFFIXES or path == Path(__file__).resolve():
                continue
            if OLD_APP_ID in path.read_text(encoding="utf-8", errors="replace"):
                offenders.append(str(path.relative_to(ROOT)))
        self.assertEqual(offenders, [], "old applicationId still present")

    def test_new_application_id_everywhere_it_is_declared(self):
        gradle = (ROOT / "shell/src/android/app/build.gradle.kts").read_text()
        self.assertIn(f'applicationId = "{APP_ID}"', gradle)
        self.assertIn(f'"appNameBase" to "{APP_NAME}"', gradle)
        builder = (ROOT / "engine/tools/build_eden_shell.py").read_text()
        self.assertIn(f'PACKAGE = "{APP_ID}"', builder)
        release = json.loads((ROOT / "RELEASE.json").read_text())
        self.assertEqual(release["application_id"], APP_ID)
        self.assertEqual(release["name"], APP_NAME)

    def test_launcher_label_is_ac_nativenx(self):
        strings = (ROOT / "shell/src/android/app/src/main/res/values/strings.xml").read_text()
        self.assertRegex(strings, rf'<string name="app_name"[^>]*>{re.escape(APP_NAME)}</string>')
        for path in (ROOT / "shell/src/android/app/src/main/res").glob("values*/*.xml"):
            values = re.findall(r">([^<]*)</string>", path.read_text(encoding="utf-8"))
            self.assertFalse([v for v in values if "Raitchi" in v],
                             f"user-visible 'Raitchi' left in {path.relative_to(ROOT)}")

    def test_credits_present(self):
        readme = (ROOT / "README.md").read_text()
        self.assertTrue(readme.startswith("# AC-NativeNX"))
        self.assertIn("based on Raitchi by igawa6", readme)
        for needle in ("Eden", "yuzu", OLD_APP_ID, "Mali", "Adreno"):
            self.assertIn(needle, readme)
        notice = (ROOT / "NOTICE.md").read_text()
        for needle in ("igawa6", "Raitchi", "Eden", "yuzu", "GPL-3.0-or-later", "AC-NativeNX"):
            self.assertIn(needle, notice)

    def test_gpl_text_unchanged(self):
        self.assertEqual(sha(ROOT / "COPYING"), UPSTREAM_COPYING_SHA256)
        self.assertEqual(sha(ROOT / "engine/COPYING"), UPSTREAM_COPYING_SHA256)

    def test_shell_overlay_manifest_matches_snapshot(self):
        # restore_shell_overlay.py refuses to build if any hash differs.
        manifest = json.loads((ROOT / "shell-overlay.json").read_text())
        stale = [row["file"] for row in manifest["files"]
                 if sha(ROOT / "shell" / row["file"]) != row["sha256"]]
        self.assertEqual(stale, [], "run tools/refresh_source_manifests.py")
        lock = json.loads((ROOT / "engine/shell.lock.json").read_text())
        self.assertEqual(lock["source_overlay"]["sha256"], sha(ROOT / "shell-overlay.json"))

    def test_source_manifest_matches_tree(self):
        manifest = json.loads((ROOT / "SOURCE-MANIFEST.json").read_text())
        stale = [row["file"] for row in manifest["files"] if sha(ROOT / row["file"]) != row["sha256"]]
        self.assertEqual(stale, [], "run tools/refresh_source_manifests.py")
        release = json.loads((ROOT / "RELEASE.json").read_text())
        self.assertEqual(release["source_manifest_sha256"], sha(ROOT / "SOURCE-MANIFEST.json"))


if __name__ == "__main__":
    unittest.main()
