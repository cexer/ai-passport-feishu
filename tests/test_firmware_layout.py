"""Prevent mounting the original /26 image cache over Passport identity data."""
import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from verify_firmware import app_contains_ram, verify_identity_window


def partition(offset, size):
    return struct.pack("<HBBII16sI", 0x50AA, 1, 0x82, offset, size, b"cache", 0)


class IdentityLayoutTest(unittest.TestCase):
    def test_grayscale_font_app_keeps_identity_and_data_offsets(self):
        text=Path('partitions.csv').read_text()
        rows={r[0].strip():r for line in text.splitlines() if line.strip() and not line.lstrip().startswith('#') for r in [line.split(',')]}
        self.assertEqual(int(rows['feishu_font'][3],0),0x500000)
        self.assertEqual(int(rows['feishu_font'][4],0),0x200000)
        self.assertEqual(rows['feishu_font'][5].strip(),'readonly')
        app=rows['factory'];self.assertEqual(int(app[3],0),0x10000)
        self.assertEqual(int(app[4],0),0x330000)
        self.assertLessEqual(int(app[3],0)+int(app[4],0),0x356000)
        self.assertEqual(int(rows['feishu_img'][3],0),0x370000)
        self.assertEqual(int(rows['feishu_img'][4],0),0x100000)

    def test_original_spiffs_is_rejected(self):
        with self.assertRaises(ValueError):
            verify_identity_window(partition(0x320000, 0x100000))

    def test_relocated_cache_keeps_original_capacity(self):
        verify_identity_window(partition(0x360000, 0x10000) + partition(0x370000, 0x100000))

    def test_exact_boundaries_are_allowed(self):
        verify_identity_window(partition(0x350000, 0x6000) + partition(0x35A000, 0x6000))

    def test_inside_window_is_rejected(self):
        with self.assertRaises(ValueError):
            verify_identity_window(partition(0x357000, 0x1000))


class InitializedRamTest(unittest.TestCase):
    def image(self):
        header=bytes([0xE9,2])+bytes(22)
        return header+struct.pack("<II",0x3fc90000,8)+b"beforeXX"+struct.pack("<II",0x3fc91000,4)+b"SQL!"

    def test_initialized_section_is_present(self):
        self.assertTrue(app_contains_ram(self.image(),0x3fc91000,b"SQL!"))

    def test_missing_or_uninitialized_section_is_rejected(self):
        self.assertFalse(app_contains_ram(self.image(),0x3fc92000,b"SQL!"))
        self.assertFalse(app_contains_ram(self.image(),0x3fc91000,bytes(4)))

    def test_truncated_segment_is_rejected(self):
        self.assertFalse(app_contains_ram(self.image()[:-1],0x3fc91000,b"SQL!"))

    def test_adjacent_segments_do_not_fake_a_contiguous_load(self):
        self.assertFalse(app_contains_ram(self.image(),0x3fc90006,b"XXSQL!"))


if __name__ == "__main__":
    unittest.main()
