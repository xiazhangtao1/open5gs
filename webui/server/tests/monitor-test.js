const assert = require('assert');
const http = require('http');
const express = require('express');
const jwt = require('jsonwebtoken');
const monitor = require('../monitor');

async function main() {
  let reads = 0, sessionTouches = 0, resolutions = 0;
  const upstream = http.createServer((req, res) => {
    assert.strictEqual(req.headers.authorization, 'Bearer internal-token');
    reads++;
    setTimeout(() => { res.setHeader('ETag', '"one"'); res.end('{"ues":100}'); }, 20);
  });
  await new Promise(resolve => upstream.listen(0, '127.0.0.1', resolve));
  const app = express();
  app.use('/api/monitor', monitor({secret: 'test-secret', token: 'internal-token',
    url: 'http://monitor.test:' + upstream.address().port,
    lookup: (hostname, options, done) => {
      assert.strictEqual(hostname, 'monitor.test'); resolutions++;
      setImmediate(() => done(null, [{address: '127.0.0.1', family: 4}]));
    }}));
  app.use((req, res) => { sessionTouches++; res.status(404).end(); });
  const server = await new Promise(resolve => { const s = app.listen(0, '127.0.0.1', () => resolve(s)); });
  const token = (claims = {}, options = {}) => jwt.sign({scope: 'monitor:read', ...claims}, 'test-secret', {
    subject: 'user-1', audience: 'xcn-monitor', issuer: 'xcn-webui', expiresIn: '5m', ...options});
  function request(access, method = 'GET', headers = {}, path = '/api/monitor/overview') {
    return new Promise((resolve, reject) => {
      const req = http.request({hostname: '127.0.0.1', port: server.address().port,
        path, method, headers: {...headers, ...(access ? {Authorization: 'Bearer ' + access} : {})}},
      res => { const chunks = []; res.on('data', c => chunks.push(c));
        res.on('end', () => resolve({status: res.statusCode, headers: res.headers, body: Buffer.concat(chunks).toString()})); });
      req.on('error', reject); req.end();
    });
  }
  try {
    assert.strictEqual((await request()).status, 401);
    assert.strictEqual((await request(token({}, {audience: 'other'}))).status, 401);
    assert.strictEqual((await request(token({scope: 'db:write'}))).status, 401);
    assert.strictEqual((await request(token({}, {expiresIn: '-1s'}))).status, 401);
    assert.strictEqual((await request(token(), 'POST')).status, 405);
    const responses = await Promise.all(Array.from({length: 50}, () => request(token())));
    assert(responses.every(r => r.status === 200 && JSON.parse(r.body).ues === 100));
    assert.strictEqual(reads, 1, 'simultaneous clients share one cache fetch');
    assert.strictEqual((await request(token(), 'GET', {'If-None-Match': '"one"'})).status, 304);
    const signalPath = '/api/monitor/signal?id=AMF%2Ftest%2F1&view=decoded';
    const details = await Promise.all(Array.from({length: 50}, () => request(token(), 'GET', {}, signalPath)));
    assert(details.every(r => r.status === 200 && r.headers['cache-control'] === 'no-store'));
    assert.strictEqual(reads, 2, 'simultaneous signal requests share only the in-flight fetch');
    assert.strictEqual((await request(token(), 'GET', {}, signalPath)).status, 200);
    assert.strictEqual(reads, 3, 'completed signal responses are not retained in the proxy cache');
    assert.strictEqual(sessionTouches, 0, 'polling never runs session middleware');
    assert.strictEqual(resolutions, 1, 'upstream address is warmed once, not looked up behind each login');
    console.log('Monitor JWT, read-only access, ETag, 50-reader coalescing, signal no-store, DNS warmup and Mongo bypass: OK');
  } finally {
    server.closeAllConnections(); upstream.closeAllConnections(); server.close(); upstream.close();
  }
}
main().catch(err => { console.error(err); process.exitCode = 1; });
