const assert = require('assert');
const http = require('http');
const express = require('express');
const jwt = require('jsonwebtoken');
const monitor = require('../monitor');

async function main() {
  let reads = 0, sessionTouches = 0;
  const upstream = http.createServer((req, res) => {
    assert.strictEqual(req.headers.authorization, 'Bearer internal-token');
    reads++;
    setTimeout(() => { res.setHeader('ETag', '"one"'); res.end('{"ues":100}'); }, 20);
  });
  await new Promise(resolve => upstream.listen(0, '127.0.0.1', resolve));
  const app = express();
  app.use('/api/monitor', monitor({secret: 'test-secret', token: 'internal-token',
    url: 'http://127.0.0.1:' + upstream.address().port}));
  app.use((req, res) => { sessionTouches++; res.status(404).end(); });
  const server = await new Promise(resolve => { const s = app.listen(0, '127.0.0.1', () => resolve(s)); });
  const token = (claims = {}, options = {}) => jwt.sign({scope: 'monitor:read', ...claims}, 'test-secret', {
    subject: 'user-1', audience: 'xcn-monitor', issuer: 'xcn-webui', expiresIn: '5m', ...options});
  function request(access, method = 'GET', headers = {}) {
    return new Promise((resolve, reject) => {
      const req = http.request({hostname: '127.0.0.1', port: server.address().port,
        path: '/api/monitor/overview', method, headers: {...headers, ...(access ? {Authorization: 'Bearer ' + access} : {})}},
      res => { const chunks = []; res.on('data', c => chunks.push(c));
        res.on('end', () => resolve({status: res.statusCode, body: Buffer.concat(chunks).toString()})); });
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
    assert.strictEqual(sessionTouches, 0, 'polling never runs session middleware');
    console.log('Monitor JWT scope/expiry, read-only access, ETag, 50-reader coalescing and Mongo bypass: OK');
  } finally {
    server.closeAllConnections(); upstream.closeAllConnections(); server.close(); upstream.close();
  }
}
main().catch(err => { console.error(err); process.exitCode = 1; });
