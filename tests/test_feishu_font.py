"""Full-CJK coverage must survive device font regeneration."""
import json
from pathlib import Path
import re
import unittest

class DeviceFontTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source=Path('main/lv_font_ai_passport_14.c').read_text(encoding='utf8')
        cls.clean=re.sub(r'/\*.*?\*/','',cls.source,flags=re.S)
        body=re.search(r'cmaps\[\]\s*=\s*\{(.*?)\n\};',cls.clean,re.S).group(1)
        cls.codes=set()
        for block in re.findall(r'\{([^{}]+)\}',body):
            start=int(re.search(r'\.range_start\s*=\s*(\d+)',block).group(1))
            length=int(re.search(r'\.range_length\s*=\s*(\d+)',block).group(1))
            name=re.search(r'\.unicode_list\s*=\s*(\w+)',block).group(1)
            if name=='NULL':cls.codes.update(range(start,start+length))
            else:
                values=re.search(r'\b'+name+r'\[\]\s*=\s*\{(.*?)\};',cls.clean,re.S).group(1)
                cls.codes.update(start+int(v,0) for v in re.findall(r'0x[0-9a-fA-F]+|\b\d+\b',values))

    def test_full_cjk_including_last_sixteen(self):
        self.assertFalse(set(range(0x4e00,0xa000))-self.codes)
        self.assertFalse(set(range(0x20,0x7f))-self.codes)
        expected=json.loads(Path('assets/fonts/SourceHanSansSC-Normal-2.005.json').read_text())
        self.assertEqual(len(self.codes),expected['codepoints'])

    def test_grayscale_decoder_and_product_metrics(self):
        for field,value in [('bpp',4),('bitmap_format',1),('line_height',17),('base_line',4)]:
            self.assertRegex(self.clean,rf'\.{field}\s*=\s*{value}\s*,')
        defaults=Path('sdkconfig.defaults').read_text()
        self.assertIn('CONFIG_LV_USE_FONT_COMPRESSED=y',defaults)
        self.assertIn('CONFIG_LV_FONT_FMT_TXT_LARGE=y',defaults)
        import hashlib
        bitmap=Path('assets/fonts/feishu-14-4bpp.bin').read_bytes()
        self.assertEqual(len(bitmap),1737446)
        expected=hashlib.sha256(bitmap).digest()
        actual=bytes(int(x,16) for x in re.search(r'expected\[32\]=\{([^}]+)',self.clean).group(1).split(','))
        self.assertEqual(actual,expected)
        self.assertIn('esp_partition_mmap(part,0,1737446',self.clean)
        self.assertIn('esp_partition_munmap(handle)',self.clean)

if __name__=='__main__':unittest.main()
