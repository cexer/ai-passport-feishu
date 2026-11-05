import assert from 'node:assert/strict';
import { onboard, userScopes, tenantScopes } from '../tools/feishu-onboarding/onboard.mjs';

let writes = 0;
const events = [];
const credentials = { client_id: 'cli_fake123', client_secret: 'fake-secret', user_info: {} };
await onboard({ register: async options => {
  assert.equal(options.createOnly, undefined);
  assert.equal(options.addons.preset, false);
  assert.deepEqual(options.addons.scopes.user, userScopes);
  assert.deepEqual(options.addons.scopes.tenant, tenantScopes);
  assert(!userScopes.includes("speech_to_text:speech"));
  options.onQRCodeReady({ url: 'https://open.feishu.cn/example?opaque=a%2Bb', expireIn: 600 });
  return credentials;
}, save: async data => {
  ++writes; assert.equal(data.app_secret, 'fake-secret');
  return 'https://accounts.feishu.cn/example?code=fake';
}, update: state => events.push(state) });
assert.equal(writes, 1);
assert.equal(credentials.client_secret, '');
assert.deepEqual(events.map(x => x.stage), ['starting', 'authorize_app', 'writing_usb', 'authorize_user']);
assert.equal(events[1].url, 'https://open.feishu.cn/example?opaque=a%2Bb');
assert(!JSON.stringify(events).includes('fake-secret'));
for (const code of ['access_denied', 'expired_token', 'abort']) {
  await assert.rejects(onboard({ register: async () => { throw { code }; },
    save: async () => { throw Error('must not write'); }, update: () => {} }), x => x.code === code);
}
for (const result of [{}, { client_id: 'bad', client_secret: 'fake' },
  { client_id: 'cli_fake', client_secret: 'fake', user_info: { tenant_brand: 'lark' } }]) {
  await assert.rejects(onboard({ register: async () => result,
    save: async () => { throw Error('must not write'); }, update: () => {} }));
}
const failed = { client_id: 'cli_fake', client_secret: 'fake-secret' };
await assert.rejects(onboard({ register: async () => failed,
  save: async () => { throw Error('usb_setup_failed'); }, update: () => {} }));
assert.equal(failed.client_secret, '');
let pending;
await onboard({ register: async () => ({ client_id: 'cli_fake', client_secret: 'fake-secret' }),
  save: async () => null, update: value => { pending = value; } });
assert.deepEqual(pending, { stage: 'pending_usb' });
console.log('Official SDK onboarding orchestration: normal, denial, expiry, abort, missing credentials, wrong brand, USB failure PASS');
