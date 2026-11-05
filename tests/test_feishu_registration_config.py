import base64
import gzip
import json
from pathlib import Path
import re
import unittest


class RegistrationConfigTest(unittest.TestCase):
    def test_compressed_addons_matches_device_user_scopes(self):
        text = Path('main/feishu_registration_config.h').read_text()
        encoded = re.search(r'#define FEISHU_APP_ADDONS "([^"]+)"', text).group(1)
        scopes = ''.join(re.findall(r'"([^"]*)"', text.split('#define FEISHU_APP_ADDONS')[0])).split()
        addons = json.loads(gzip.decompress(base64.urlsafe_b64decode(encoded + '=' * (-len(encoded) % 4))))
        self.assertEqual(addons, {'preset': False, 'scopes': {'user': scopes, 'tenant': ['speech_to_text:speech']}})
        self.assertLess(len(encoded), 300)


if __name__ == '__main__': unittest.main()
