#!/usr/bin/env node
// Host-only official app registration. No cloud relay or firmware SDK overhead.
import { registerApp } from '@larksuiteoapi/node-sdk';
import { spawn } from 'node:child_process';
import { writeFileSync, renameSync, mkdirSync } from 'node:fs';
import { resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

export const userScopes = [
  'im:chat:readonly', 'im:message', 'im:message.p2p_msg:get_as_user',
  'im:message.group_msg:get_as_user', 'im:message.send_as_user',
  'offline_access',
];

export const tenantScopes = ['speech_to_text:speech'];

export async function onboard({ register = registerApp, save, update, signal }) {
  update({ stage: 'starting' });
  const result = await register({
    source: 'ai-passport', signal,
    appPreset: { name: 'AI Passport 飞书终端', desc: '个人设备上的飞书消息阅读与语音输入' },
    // Keep only the bot base capability and the permissions actually requested
    // by the device. Allow selecting an existing app (no createOnly).
    addons: { preset: false, scopes: { user: userScopes, tenant: tenantScopes } },
    onQRCodeReady({ url, expireIn }) {
      update({ stage: 'authorize_app', url, expiresAt: Date.now() + expireIn * 1000 });
    },
  });
  if (result.user_info?.tenant_brand === 'lark') throw new Error('lark_unsupported');
  if (!/^cli_[A-Za-z0-9]+$/.test(result.client_id ?? '') || !result.client_secret) {
    throw new Error('incomplete_credentials');
  }
  update({ stage: 'writing_usb' });
  try {
    const url = await save({ app_id: result.client_id, app_secret: result.client_secret });
    update(url ? { stage: 'authorize_user', url } : { stage: 'pending_usb' });
  } finally {
    result.client_secret = '';
  }
}

function saveUsb(python, port, credentials) {
  return new Promise((accept, reject) => {
    const script = fileURLToPath(new URL('../feishu_provision.py', import.meta.url));
    const child = spawn(python, [script, '--port', port, '--json-stdin',
      '--wait-auth-url', '--timeout', '90'], { windowsHide: true, stdio: ['pipe', 'pipe', 'pipe'] });
    let output = '';
    child.stdout.on('data', data => { output = (output + data).slice(-4096); });
    // Never relay arbitrary subprocess/SDK exceptions: they may echo credentials.
    child.stderr.resume();
    child.on('error', () => reject(new Error('usb_process_failed')));
    child.stdin.on('error', () => reject(new Error('usb_input_failed')));
    child.on('close', code => {
      if (code !== 0) return reject(new Error('usb_setup_failed'));
      try {
        const response = JSON.parse(output);
        const url = new URL(response.url);
        if (response.stage !== 'authorize_user' || url.protocol !== 'https:' ||
            !['accounts.feishu.cn', 'open.feishu.cn'].includes(url.hostname)) throw new Error();
        accept(response.url);
      } catch { reject(new Error('invalid_device_url')); }
    });
    child.stdin.end(JSON.stringify(credentials));
  });
}

function protectPending(python, credentials, path) {
  return new Promise((accept, reject) => {
    const script = fileURLToPath(new URL('../feishu_provision.py', import.meta.url));
    const child = spawn(python, [script, '--protect-stdin'],
      { windowsHide: true, stdio: ['pipe', 'pipe', 'pipe'] });
    let output = '';
    child.stdout.on('data', data => { output = (output + data).slice(-16384); });
    child.stderr.resume();
    child.on('error', () => reject(new Error('pending_protection_failed')));
    child.stdin.on('error', () => reject(new Error('pending_protection_failed')));
    child.on('close', code => {
      if (code !== 0 || !/^[A-Za-z0-9+/=]+$/.test(output.trim())) {
        return reject(new Error('pending_protection_failed'));
      }
      try {
        mkdirSync(dirname(resolve(path)), { recursive: true });
        writeFileSync(path, output.trim(), { flag: 'wx', mode: 0o600 });
        accept();
      } catch { reject(new Error('pending_protection_failed')); }
    });
    child.stdin.end(JSON.stringify(credentials));
  });
}

async function main() {
  const args = process.argv.slice(2);
  const option = key => { const n = args.indexOf(key); return n >= 0 ? args[n + 1] : undefined; };
  const python = option('--python');
  const port = option('--port');
  const stateFile = option('--status-file');
  const pendingFile = option('--pending-file');
  if (!python || !port || !stateFile) {
    console.error('Usage: node onboard.mjs --python <python> --port <USB> --status-file <local-file>');
    process.exitCode = 2;
    return;
  }
  const controller = new AbortController();
  process.on('SIGINT', () => controller.abort());
  // Also bound network requests/SDK polling if its transport never resolves.
  const timer = setTimeout(() => controller.abort(), 11 * 60 * 1000);
  timer.unref();
  const update = state => {
    const path = resolve(stateFile);
    mkdirSync(dirname(path), { recursive: true });
    const value = JSON.stringify({ ...state, updatedAt: new Date().toISOString() });
    writeFileSync(path + '.tmp', value, { mode: 0o600 });
    renameSync(path + '.tmp', path);
    console.log(value); // Only explicit safe stages and user-facing URLs.
  };
  try {
    await onboard({ save: async credentials => {
      if (pendingFile) await protectPending(python, credentials, pendingFile);
      try {
        const url = await saveUsb(python, port, credentials);
        // The device now holds a candidate in RAM until the user grant commits.
        // Retain the optional encrypted recovery file until binding is verified.
        return url;
      }
      catch (error) {
        if (!pendingFile) throw error;
        // Preserve encrypted credentials when Wi-Fi/USB is not ready. Resume
        // with feishu_provision.py --encrypted-file, without another app login.
        return null;
      }
    }, update,
      signal: controller.signal });
  } catch (error) {
    const allowed = ['access_denied', 'expired_token', 'abort', 'lark_unsupported',
      'incomplete_credentials', 'usb_process_failed', 'usb_input_failed', 'usb_setup_failed', 'invalid_device_url'];
    const code = error?.code ?? error?.message;
    update({ stage: 'failed', error: allowed.includes(code) ? code : 'registration_failed' });
    process.exitCode = 1;
  } finally { clearTimeout(timer); }
}

if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) await main();
