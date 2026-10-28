import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import check_button_font


FONT = Path(__file__).resolve().parents[1] / "ports/shared-android/assets/fonts/shared_5.ttf"


class ButtonFontTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.data = FONT.read_bytes()

    def test_packaged_font_is_small_and_covers_all_button_codepoints(self):
        result = check_button_font.check_font(self.data, str(FONT))
        self.assertLessEqual(result["bytes"], 64 * 1024)
        self.assertEqual(len(result["required_codepoints"]), 41)
        self.assertTrue(check_button_font.has_codepoint(self.data, 0xE0E4))  # L
        self.assertTrue(check_button_font.has_codepoint(self.data, 0xE0E5))  # R

    def test_unmapped_neighbor_is_not_accepted(self):
        self.assertFalse(check_button_font.has_codepoint(self.data, 0xE0EB))
        self.assertFalse(check_button_font.has_codepoint(self.data, 0xE0F1))

    def test_truncated_or_oversized_font_is_rejected(self):
        with self.assertRaises(ValueError):
            check_button_font.check_font(self.data[:64], "truncated")
        with self.assertRaises(ValueError):
            check_button_font.check_font(self.data + b"\0" * (64 * 1024), "oversized")


if __name__ == "__main__":
    unittest.main()
