import base64
import contextlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
import feishu_voice_login as login

class VoiceLoginTest(unittest.TestCase):
 def test_first_party_only_and_no_header_injection(self):
  s=login.session_for({})
  s.cookies.set('session','fake-valid',domain='.feishu.cn')
  s.cookies.set('sl_session','fake-foreign',domain='attacker.invalid')
  c=login.credential(s)
  self.assertEqual(c['cookie'],'session=fake-valid')
  s.cookies.set('session','bad\r\nheader',domain='.feishu.cn')
  self.assertIsNone(login.credential(s))
 def test_expiry_and_cookie_bound(self):
  s=login.session_for({})
  s.cookies.set('session','fake-session',domain='.feishu.cn')
  data=base64.urlsafe_b64encode(json.dumps({'exp':1800000000}).encode()).decode().rstrip('=')
  s.cookies.set('sl_session','fake.'+data+'.fake',domain='.feishu.cn')
  self.assertEqual(login.credential(s)['expires_at'],1800000000)
  s.cookies.set('session','x'*4097,domain='.feishu.cn')
  self.assertIsNone(login.credential(s))
 def test_pending_state_is_sealed_and_errors_are_redacted(self):
  value={'cookie':'sensitive-synthetic-value'}
  def seal(data,decrypt=False):return data[7:] if decrypt else b'sealed:'+data
  with tempfile.TemporaryDirectory() as directory,patch.object(login,'transform',seal):
   path=Path(directory)/'state.dpapi';login.write_state(path,value)
   self.assertTrue(path.read_bytes().startswith(b'sealed:'))
   self.assertEqual(login.read_state(path),value)
  output=io.StringIO()
  with patch.object(sys,'argv',['login','--begin']),patch.object(login,'begin',side_effect=RuntimeError('session=secret')),contextlib.redirect_stdout(output):
   with self.assertRaises(SystemExit):login.main()
  self.assertNotIn('secret',output.getvalue())
  self.assertEqual(json.loads(output.getvalue())['error_type'],'RuntimeError')

if __name__=='__main__':unittest.main()
