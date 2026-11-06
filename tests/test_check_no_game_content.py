"""Tests for tools/check_no_game_content.py (synthetic files only)."""
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

GUARD = Path(__file__).resolve().parents[1] / "tools" / "check_no_game_content.py"


def run_guard(root: Path):
    return subprocess.run([sys.executable, str(GUARD), str(root)],
                          capture_output=True, text=True)


class GuardTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)

    def tearDown(self):
        self._tmp.cleanup()

    def put(self, rel, data=b"x"):
        p = self.root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)
        return p

    def assert_refused(self, rel):
        r = run_guard(self.root)
        self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
        self.assertIn(rel, r.stderr)  # message names the offending path

    def test_clean_tree_passes(self):
        self.put("README.md", b"hello")
        self.put("src/main.c", b"int main(){}")
        r = run_guard(self.root)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("OK", r.stdout)

    def test_flags_nsp(self):
        self.put("foo.nsp")
        self.assert_refused("foo.nsp")

    def test_flags_other_game_extensions(self):
        for ext in (".xci", ".nsz", ".xcz", ".nca", ".tik"):
            with self.subTest(ext=ext):
                self.put("dump/game" + ext)
                self.assert_refused("dump/game" + ext)
                (self.root / ("dump/game" + ext)).unlink()

    def test_extension_check_is_case_insensitive(self):
        self.put("GAME.NSP")
        self.assert_refused("GAME.NSP")

    def test_flags_prod_keys(self):
        self.put("keys/prod.keys")
        self.assert_refused("keys/prod.keys")

    def test_flags_title_and_any_keys_file(self):
        self.put("title.keys")
        self.put("my.keys")
        r = run_guard(self.root)
        self.assertEqual(r.returncode, 1)
        self.assertIn("title.keys", r.stderr)
        self.assertIn("my.keys", r.stderr)

    def test_flags_romfs_and_exefs_dirs(self):
        self.put("romfs/a.bin")
        self.put("x/exefs/main")
        r = run_guard(self.root)
        self.assertEqual(r.returncode, 1)
        self.assertIn("romfs/a.bin", r.stderr)
        self.assertIn("x/exefs/main", r.stderr)

    def test_flags_big_file(self):
        p = self.root / "blob.dat"
        with open(p, "wb") as f:  # sparse 60 MB
            f.truncate(60 * 1024 * 1024)
        self.assert_refused("blob.dat")

    def test_file_under_limit_passes(self):
        p = self.root / "ok.dat"
        with open(p, "wb") as f:
            f.truncate(10 * 1024 * 1024)
        self.assertEqual(run_guard(self.root).returncode, 0)

    def test_git_dir_is_ignored(self):
        self.put(".git/objects/foo.nsp")
        self.assertEqual(run_guard(self.root).returncode, 0)

    def test_message_explains_what_to_do(self):
        self.put("a.xci")
        r = run_guard(self.root)
        self.assertIn("a.xci", r.stderr)
        self.assertIn("remove", r.stderr.lower())


if __name__ == "__main__":
    unittest.main()
