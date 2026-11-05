"""Bounded USB setup frames and phase handshake with a simulated serial peer."""
import base64
import json
from pathlib import Path
import sys
import unittest
from types import SimpleNamespace
from unittest.mock import patch
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from feishu_provision import build_frame, build_wifi_frame, provision


class Port:
    def __init__(self, lines):
        self.lines = list(lines)
        self.writes = []
    def __enter__(self): return self
    def __exit__(self, *args): pass
    def write(self, value): self.writes.append(value)
    def readline(self): return self.lines.pop(0) if self.lines else b''


class ProvisionTests(unittest.TestCase):
    def setUp(self):
        self.serial = patch.dict(sys.modules, {'serial': SimpleNamespace(
            Serial=None, SerialTimeoutException=TimeoutError)})
        self.serial.start()
        self.addCleanup(self.serial.stop)

    def test_app_frame_and_invalid_fields(self):
        frame = build_frame('cli_example123', 'test-secret')
        prefix, encoded = frame.rstrip(b'\n').split(b' ', 1)
        self.assertEqual(prefix, b'FAP-FEISHU/1')
        self.assertEqual(json.loads(base64.b64decode(encoded)),
                         {'app_id': 'cli_example123', 'app_secret': 'test-secret'})
        for app, secret in [('bad-id', 'test'), ('cli_example123', ''),
                            ('cli_example123', 'bad\0secret')]:
            with self.assertRaises(ValueError): build_frame(app, secret)

    def test_utf8_bounds_and_empty_wifi_password(self):
        frame = build_wifi_frame('中' * 10, '')
        self.assertEqual(json.loads(base64.b64decode(frame.split()[1]))['password'], '')
        for ssid, password in [('中' * 11, ''), ('', ''), ('a', 'x' * 65), ('a\0b', '')]:
            with self.assertRaises(ValueError): build_wifi_frame(ssid, password)
        with self.assertRaises(ValueError): build_frame('cli_fake', '中' * 43)

    def test_matching_phase_and_device_auth_url(self):
        frame = build_frame('cli_fake', 'fake-secret')
        peer = Port([b'FAP-FEISHU/1 READY\n', b'FAP-FEISHU/1 OK\n',
                     b'FAP-SETUP/1 AUTH https://accounts.feishu.cn/example?opaque=a%2Bb\n'])
        with patch('serial.Serial', return_value=peer):
            self.assertEqual(provision('fake', frame, 2, True),
                             'https://accounts.feishu.cn/example?opaque=a%2Bb')
        self.assertEqual(peer.writes, [b'FAP-SETUP/1 PING\n', frame])

    def test_wrong_phase_never_sends_secret(self):
        peer = Port([b'FAP-WIFI/1 READY\n'])
        with patch('serial.Serial', return_value=peer), self.assertRaises(RuntimeError):
            provision('fake', build_frame('cli_fake', 'fake-secret'), 2)
        self.assertEqual(peer.writes, [b'FAP-SETUP/1 PING\n'])

    def test_bad_url_and_rejection(self):
        for tail in [b'FAP-FEISHU/1 ERROR\n',
                     b'FAP-SETUP/1 AUTH https://evil.invalid/example\n']:
            peer = Port([b'FAP-FEISHU/1 READY\n', b'FAP-FEISHU/1 OK\n', tail])
            with patch('serial.Serial', return_value=peer), self.assertRaises(RuntimeError):
                provision('fake', build_frame('cli_fake', 'fake-secret'), 2, True)


class MobilePortalContractTest(unittest.TestCase):
    def test_first_run_only_requires_wifi(self):
        source = Path('main/setup_portal.c').read_text(encoding='utf-8')
        wifi_form = source[source.index('    char *page = calloc'):source.index('static int from_hex')]
        self.assertNotIn('name=app_id', wifi_form)
        self.assertNotIn('name=app_secret', wifi_form)
        self.assertIn('select name=ssid required', wifi_form)

    def test_private_app_hotspot_is_password_protected(self):
        source = Path('main/setup_portal.c').read_text(encoding='utf-8')
        self.assertIn('WIFI_AUTH_WPA2_PSK', source)
        self.assertIn('random_password(info->password)', source)

    def test_invalid_client_reopens_owner_app_setup(self):
        binding = Path('main/feishu_binding.c').read_text(encoding='utf-8')
        onboarding = Path('main/product_onboarding.c').read_text(encoding='utf-8')
        self.assertIn('strcmp(oauth_error, "invalid_client") == 0', binding)
        self.assertNotIn('feishu_store_clear_credentials()', onboarding)
        self.assertIn('feishu_registration_begin_cancelable(', onboarding)
        self.assertIn('goto configure_owner_app;', onboarding)

    def test_user_message_read_scopes_are_requested(self):
        binding = Path('main/feishu_registration_config.h').read_text(encoding='utf-8')
        for scope in ['im:chat:readonly', 'im:message.p2p_msg:get_as_user',
                      'im:message.group_msg:get_as_user']:
            self.assertIn(scope, binding)


if __name__ == '__main__': unittest.main()
