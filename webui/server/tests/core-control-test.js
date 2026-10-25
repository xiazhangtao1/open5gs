const assert = require('assert');
const http = require('http');
const http2 = require('http2');
const express = require('express');
const session = require('express-session');
const csrf = require('lusca').csrf();
const admin = require('../core-admin');
const control = require('../core-control');
const {bitrate, validateSessionConfig} = require('../../lib/session-control');

async function main() {
  let latestRole = 'admin', sent = [], clock = Date.now(), held, stall = false, sessionStatus = 202;
  const version = '11111111-1111-1111-1111-111111111111';
  const pcf = http2.createServer();
  pcf.on('stream', (stream, headers) => {
    let body = '';
    stream.on('data', b => { body += b; });
    stream.on('error', () => {});
    stream.on('end', () => {
      sent.push({method: headers[':method'], path: headers[':path'], body: body ? JSON.parse(body) : null, version: headers['if-match'], operationId: headers['x-operation-id']});
      if (stall && headers[':method'] !== 'GET') { held = stream; return; }
      if (headers[':method'] === 'GET') {
        stream.respond({':status': 200, 'content-type': 'application/json'});
        stream.end(JSON.stringify({controlVersion: version, supi: 'imsi-460110000000100', pduSessionId: 1,
          bearers: [{appSessionId: '7', pccRules: []}]}));
      } else if (headers[':method'] === 'PATCH' && headers[':path'].includes('/xcn-session-control/')) {
        stream.respond({':status': sessionStatus, 'content-type': 'application/json'});
        stream.end(JSON.stringify({status: sessionStatus === 200 ? 'unchanged' : 'accepted'}));
      } else { stream.respond({':status': 204}); stream.end(); }
    });
  });
  await new Promise(resolve => pcf.listen(0, '127.0.0.1', resolve));
  const origin = 'http://127.0.0.1:' + pcf.address().port;
  const records = new Map();
  const audit = {
    async claim(id, record) {
      if (records.has('q' + id)) {
        if (records.get('q' + id).fingerprint !== record.fingerprint) throw Object.assign(new Error('duplicate'), {status: 409});
        return records.get('r' + id) || {status: 409, data: {state: 'unknown'}};
      }
      records.set('q' + id, record); return null;
    },
    async result(id, record) { records.set('r' + id, record); },
    async list() { return [...records.entries()].filter(([k]) => k[0] === 'r').map(([,v]) => v); }
  };
  const Account = {findById() { return {select() { return {lean: async () => ({username: 'tester', roles: [latestRole]})}; }}; }};
  const app = express();
  app.use(express.json()); app.use(session({secret: 'test-only', resave: false, saveUninitialized: true})); app.use(csrf);
  app.use((req, res, next) => { if (req.get('X-Test-User')) req.user = {_id: 'test-id', roles: ['admin']}; next(); });
  app.get('/csrf', (req, res) => res.json({token: res.locals._csrf}));
  const router = control({pcf: origin, amf: origin, admin: admin(Account), audit, now: () => clock,
    users: async () => ['imsi-460110000000100']});
  app.use('/api/core-control', router);
  // The same role middleware guards account writes, independent of UI/JWT claims.
  app.patch('/api/db/Account/admin', admin(Account), (req, res) => res.json({ok: true}));
  const server = await new Promise(resolve => { const s = app.listen(0, '127.0.0.1', () => resolve(s)); });
  let cookie, token, serial = 0;
  async function request(path, method = 'GET', body, headers = {}, loggedIn = true) {
    return new Promise((resolve, reject) => {
      const req = http.request({hostname: '127.0.0.1', port: server.address().port, path, method,
        headers: {'Content-Type': 'application/json', ...(cookie ? {Cookie: cookie} : {}),
          ...(token ? {'X-CSRF-TOKEN': token} : {}), ...(loggedIn ? {'X-Test-User': '1'} : {}), ...headers}}, res => {
        const chunks = []; res.on('data', b => chunks.push(b)); res.on('end', () => {
          let data; try { data = JSON.parse(Buffer.concat(chunks).toString()); } catch(e) { data = {}; }
          resolve({status: res.statusCode, data, cookie: (res.headers['set-cookie'] || [])[0]});
        });
      });
      req.on('error', reject); req.end(body === undefined ? undefined : JSON.stringify(body));
    });
  }
  const root = '/api/core-control', target = root + '/sessions/imsi-460110000000100/1';
  const rule = {flowDescriptions: ['permit out ip from any to assigned'], qos: {'5qi': 9}};
  const config = {defaultQos: {'5qi': 8, priorityLevel: 70, arp: {priorityLevel: 8,
    preemptionCapability: 'NOT_PREEMPT', preemptionVulnerability: 'PREEMPTABLE'}},
    sessionAmbr: {uplink: '10 Mbps', downlink: '20 Mbps'}};
  const write = (path, method, body, extra = {}) => {
    clock += 1000;
    return request(path, method, body, {'X-Operation-ID': 'operation-test-' + (++serial), 'If-Match': '"' + version + '"', ...extra});
  };
  try {
    assert.strictEqual(bitrate('0.000001 Tbps'), 1000000);
    for (const value of ['0 Mbps', '1 bps', '65535001 bps', 'NaN Mbps', '1e3 Mbps', '4.000001 Tbps'])
      assert.throws(() => bitrate(value));
    assert.deepStrictEqual(validateSessionConfig(config), config);
    const c = await request('/csrf'); cookie = c.cookie.split(';')[0]; token = c.data.token;
    assert.strictEqual((await request(target, 'GET', undefined, {}, false)).status, 401);
    latestRole = 'user';
    assert.strictEqual((await request(target)).status, 403, 'fresh role beats stale admin JWT claim');
    assert.strictEqual((await write('/api/db/Account/admin', 'PATCH', {roles: ['admin']})).status, 403);
    assert.strictEqual((await write(target, 'PATCH', config)).status, 403);
    latestRole = 'admin';
    assert.strictEqual((await write(target + '/bearers', 'POST', rule, {'X-CSRF-TOKEN': 'invalid'})).status, 403);
    assert.strictEqual((await request(target)).status, 200);
    assert.strictEqual((await write(target + '/bearers', 'POST', rule, {'If-Match': ''})).status, 428);
    let before = sent.length;
    assert.strictEqual((await write(target + '/bearers', 'POST', {...rule, ueIp: '127.0.0.1'})).status, 400);
    assert.strictEqual(sent.length, before, 'selectors cannot override the URL target');
    const id = 'repeat-operation-00000001';
    const first = await write(target + '/bearers', 'POST', rule, {'X-Operation-ID': id});
    assert.strictEqual(first.status, 202); assert.strictEqual(first.data.state, 'accepted');
    before = sent.length;
    assert.strictEqual((await write(target + '/bearers', 'POST', rule, {'X-Operation-ID': id})).status, 202);
    assert.strictEqual(sent.length, before, 'same operation is not sent twice');
    assert.strictEqual((await write(target + '/bearers', 'POST', {qos: {'5qi': 8}, flowDescriptions: rule.flowDescriptions}, {'X-Operation-ID': id})).status, 409);
    assert.strictEqual((await write(target + '/bearers/99', 'DELETE')).status, 404);
    assert.strictEqual((await write(target + '/bearers/7', 'PATCH', rule)).status, 202);
    assert.strictEqual((await write(target, 'PATCH', config, {'X-CSRF-TOKEN': 'invalid'})).status, 403);
    assert.strictEqual((await write(target, 'PATCH', config, {'If-Match': ''})).status, 428);
    before = sent.length;
    for (const invalid of [{...config, supi: 'imsi-460110000000999'},
      {...config, defaultQos: {...config.defaultQos, '5qi': 1}},
      {...config, defaultQos: {...config.defaultQos, priorityLevel: 128}},
      {...config, sessionAmbr: {uplink: '1 bps', downlink: '20 Mbps'}}])
      assert.strictEqual((await write(target, 'PATCH', invalid)).status, 400);
    assert.strictEqual(sent.length, before, 'invalid session changes never reach the core');
    const changed = await write(target, 'PATCH', config);
    assert.strictEqual(changed.status, 202);
    assert.strictEqual(sent[sent.length - 1].operationId, changed.data.operationId);
    assert.deepStrictEqual(sent[sent.length - 1].body, config);
    sessionStatus = 200;
    const noop = await write(target, 'PATCH', config);
    assert.strictEqual(noop.status, 200); assert.strictEqual(noop.data.state, 'unchanged');
    for (const status of [409, 412, 501, 503]) {
      sessionStatus = status;
      const error = await write(target, 'PATCH', config);
      assert.strictEqual(error.status, status);
      assert.strictEqual(error.data.state, status >= 500 && status !== 501 ? 'unknown' : 'failed');
    }
    sessionStatus = 202;
    assert.strictEqual((await write(target, 'DELETE')).status, 202);
    assert.strictEqual(sent[sent.length - 1].path, '/xcn-session-control/v1/sessions/imsi-460110000000100/1');
    assert.strictEqual((await write(root + '/ues/imsi-460110000000100', 'DELETE')).status, 202);
    assert.strictEqual((await request(root + '/ues')).data.supis.length, 1);
    assert((await request(root + '/operations')).data.length > 0);
    stall = true;
    const pending = write(target, 'DELETE');
    await new Promise(resolve => setTimeout(resolve, 60));
    assert.strictEqual((await write(target + '/bearers', 'POST', rule)).status, 429, 'same UE is locked');
    held.respond({':status': 204}); held.end();
    assert.strictEqual((await pending).status, 202);
    const timeout = await write(target, 'DELETE');
    assert.strictEqual(timeout.status, 504); assert.strictEqual(timeout.data.state, 'unknown');
    console.log('Control API: current roles, CSRF, scoped targets, validation, conditional writes, deduplication, concurrency, timeout and audit: OK');
  } finally { router.close(); server.closeAllConnections(); server.close(); pcf.close(); }
}
main().catch(e => { console.error(e); process.exitCode = 1; });
