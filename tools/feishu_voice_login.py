"""Obtain a private Feishu web session for the internal-ASR device experiment.

Credentials stay in a CurrentUser DPAPI file; stdout never contains cookies.
Run with the project's ESP-IDF Python (requests is already installed there).
"""
import argparse
import base64
import json
import time
from pathlib import Path
import requests
from windows_secrets import transform

ORIGIN = 'https://accounts.feishu.cn'
REDIRECT = 'https://open-dev.feishu.cn/next/messenger'
UA = 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/150.0.0.0 Safari/537.36'

def write_state(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    payload = json.dumps(value, separators=(',', ':')).encode('utf8')
    sealed = transform(payload)
    if transform(sealed, True) != payload:
        raise RuntimeError('private_state_verification_failed')
    temporary = path.with_suffix(path.suffix + '.pending')
    temporary.write_bytes(sealed)
    temporary.replace(path)

def read_state(path):
    return json.loads(transform(path.read_bytes(), True))

def session_for(state):
    s = requests.Session()
    s.trust_env = False
    s.headers.update({'User-Agent': UA, 'Content-Type': 'application/json',
        'X-App-Id': '1', 'X-Api-Version': '1.0.8', 'X-Device-Info': 'platform=websdk',
        'X-Terminal-Type': '2', 'Origin': ORIGIN,
        'Referer': ORIGIN + '/accounts/page/login?app_id=1'})
    if state.get('flow_key'):
        s.headers['X-Flow-Key'] = state['flow_key']
    for cookie in state.get('cookies', []):
        s.cookies.set_cookie(requests.cookies.create_cookie(**cookie))
    return s

def collect(s):
    return [{'name': c.name, 'value': c.value, 'domain': c.domain, 'path': c.path,
             'secure': c.secure, 'expires': c.expires} for c in s.cookies]

def credential(s):
    values = {c.name: c.value for c in s.cookies
              if (c.domain.lstrip('.') == 'feishu.cn' or c.domain.endswith('.feishu.cn')) and c.name in ('session', 'session_list', 'sl_session')}
    if not values.get('session') or any(any(ord(c)<33 or ord(c)>126 for c in v) for v in values.values()):
        return None
    expiry = 0
    if values.get('sl_session'):
        try:
            part = values['sl_session'].split('.')[1]
            expiry = int(json.loads(base64.urlsafe_b64decode(part + '=' * (-len(part) % 4)))['exp'])
        except (ValueError, KeyError, IndexError):
            expiry = 0
    header='; '.join(k + '=' + values[k] for k in ('session', 'session_list', 'sl_session') if k in values)
    if len(header)>4096:return None
    return {'cookie': header,
            'expires_at': expiry, 'obtained_at': int(time.time()), 'backend': 'internal-1305-v8.4.7'}

def begin(path):
    state = {'cookies': [], 'created_at': int(time.time()), 'phase': 'waiting'}
    s = session_for(state)
    response = s.post(ORIGIN + '/accounts/qrlogin/init', json={'redirect_uri': REDIRECT}, timeout=15)
    response.raise_for_status()
    data = response.json()
    if data.get('code') != 0:
        raise RuntimeError('login_init_rejected')
    state.update(token=data['data']['step_info']['token'], flow_key=response.headers.get('x-flow-key', ''), cookies=collect(s))
    if not state['flow_key']:
        raise RuntimeError('login_flow_key_missing')
    write_state(path, state)
    # This is the intended single-use QR payload, never a cookie/session value.
    print(json.dumps({'phase': 'waiting', 'qr_content': json.dumps({'qrlogin': {'token': state['token']}}, separators=(',', ':')),
                      'state_file': str(path), 'observation_window_seconds': 180}, ensure_ascii=False))

def wait(path, output, timeout):
    state = read_state(path)
    if state.get('phase') == 'authenticated':
        print(json.dumps({'phase': 'authenticated', 'private_output': str(output)})); return
    s = session_for(state)
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        response = s.post(ORIGIN + '/accounts/qrlogin/polling', json={}, timeout=15)
        response.raise_for_status()
        result = response.json().get('data') or {}
        status = (result.get('step_info') or {}).get('status')
        state['cookies'] = collect(s)
        state['last_poll_at'] = int(time.time())
        write_state(path, state)
        if status in (3, 4, 5):
            print(json.dumps({'phase': 'cancelled_or_expired', 'status': status})); return
        value = credential(s)
        if value:
            write_state(output, value)
            state['phase'] = 'authenticated'
            write_state(path, state)
            print(json.dumps({'phase': 'authenticated', 'private_output': str(output),
                              'expires_at': value['expires_at'], 'cookie_bytes': len(value['cookie'])})); return
        time.sleep(2)
    print(json.dumps({'phase': 'timeout', 'resumable': True}))

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--state', type=Path, default=Path('build/internal-asr-login.dpapi'))
    p.add_argument('--output', type=Path, default=Path('build/internal-asr-session.dpapi'))
    p.add_argument('--timeout', type=int, default=180)
    p.add_argument('--begin', action='store_true')
    p.add_argument('--wait', action='store_true')
    args = p.parse_args()
    if args.begin == args.wait:
        p.error('choose --begin or --wait')
    try:
        if args.begin: begin(args.state)
        else: wait(args.state, args.output, min(max(args.timeout, 1), 600))
    except Exception as exc:
        # requests/JSON errors may carry URLs or credential data; report a type only.
        print(json.dumps({'phase': 'failed', 'error_type': type(exc).__name__}))
        raise SystemExit(1)

if __name__ == '__main__':
    main()
