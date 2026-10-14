// Run only against a disposable MongoDB database; see docs/webui-framed-routes.md.
const assert = require('node:assert/strict');
const { test, describe, before, after } = require('node:test');
const express = require('express');
const mongoose = require('mongoose');
const Subscriber = require('../server/models/subscriber');
const dbRoutes = require('../server/routes/db');
const { IPV4_CIDR_PATTERN } = require('../src/helpers/framed-routes');

const uri = process.env.WEBUI_TEST_DB_URI;
assert.ok(uri && /^\/webui_framed_routes_test(?:_[a-zA-Z0-9]+)?$/.test(new URL(uri).pathname),
  'Set WEBUI_TEST_DB_URI to a disposable webui_framed_routes_test database');

let server;
let baseUrl;
const fixture = (imsi, routes) => ({
  imsi,
  slice: [{ sst: 1, sd: '010101', session: [{
    name: 'cmnet', ...(routes === undefined ? {} : { ipv4_framed_routes: routes })
  }] }]
});
const routesOf = subscriber => subscriber.slice[0].session[0].ipv4_framed_routes;
const validRoutes = ['192.168.0.0/16', '0.0.0.0/0', '255.255.255.255/32'];
const invalidRoutes = [
  '', '192.168.0.0', '256.168.0.0/16', '192.168.0.0/33', '192.168.0.0/-1',
  '192.168.0.0/016', '192.168.00.0/16', '2001:db8::/64', '192.168.0.0/16\n',
  ' 192.168.0.0/16', '192.168.0.0/16 ', null, 123, {}
];

async function request(method, path = '', body) {
  const response = await fetch(baseUrl + '/Subscriber' + path, {
    method,
    headers: { 'Content-Type': 'application/json' },
    ...(body === undefined ? {} : { body: JSON.stringify(body) })
  });
  const result = await response.text();
  return { status: response.status, data: result ? JSON.parse(result) : undefined };
}

describe('IPv4 framed routes', () => {
  before(async () => {
    await mongoose.connect(uri, { useNewUrlParser: true, useUnifiedTopology: true });
    const app = express();
    app.use(express.json());
    app.use('/api/db', dbRoutes);
    server = await new Promise(resolve => {
      const listener = app.listen(0, '127.0.0.1', () => resolve(listener));
    });
    baseUrl = 'http://127.0.0.1:' + server.address().port + '/api/db';
  });

  after(async () => {
    if (server) await new Promise(resolve => server.close(resolve));
    if (mongoose.connection.readyState === 1) await mongoose.connection.dropDatabase();
    await mongoose.disconnect();
  });

  test('form pattern accepts CIDR boundaries and rejects invalid strings', () => {
    const pattern = new RegExp(IPV4_CIDR_PATTERN);
    validRoutes.forEach(route => assert.ok(pattern.test(route), route));
    invalidRoutes.filter(route => typeof route === 'string').forEach(route =>
      assert.equal(pattern.test(route), false, route));
  });

  test('model leaves legacy sessions unchanged and accepts empty or eight routes', async () => {
    const legacy = new Subscriber(fixture('001010000000001'));
    await legacy.validate();
    assert.equal(routesOf(legacy.toObject()), undefined);
    for (const routes of [[], validRoutes, Array(8).fill('192.168.0.0/16')]) {
      const subscriber = new Subscriber(fixture('001010000000001', routes));
      await subscriber.validate();
      assert.deepEqual(Array.from(routesOf(subscriber)), routes);
    }
  });

  test('model rejects invalid entries, non-arrays and more than eight routes', async () => {
    const invalidLists = invalidRoutes.map(route => [route]).concat([
      Array(9).fill('192.168.0.0/16'), null, '192.168.0.0/16', {}
    ]);
    for (const routes of invalidLists) {
      await assert.rejects(new Subscriber(fixture('001010000000001', routes)).validate(),
        undefined, JSON.stringify(routes));
    }
  });

  test('API creates, reads, updates and clears routes without changing other sessions', async () => {
    const body = fixture('001010000000002', validRoutes);
    body.slice[0].session.push({ name: 'internet', ipv4_framed_routes: ['172.16.0.0/12'] });
    body.slice.push({ sst: 1, sd: '020202', session: [{
      name: 'cmnet', ipv4_framed_routes: ['10.10.0.0/16']
    }] });
    const created = await request('POST', '', body);
    assert.equal(created.status, 201);
    assert.deepEqual(routesOf(created.data), validRoutes);
    const read = await request('GET', '/' + body.imsi);
    assert.equal(read.status, 200);
    assert.deepEqual(routesOf(read.data), validRoutes);

    read.data.subscriber_status = 1;
    const edited = await request('PATCH', '/' + body.imsi, read.data);
    assert.equal(edited.status, 200);
    assert.deepEqual(routesOf(edited.data), validRoutes);
    edited.data.slice[0].session[0].ipv4_framed_routes = ['10.20.0.0/16'];
    const changed = await request('PATCH', '/' + body.imsi, { slice: edited.data.slice });
    assert.equal(changed.status, 200);
    assert.deepEqual(routesOf(changed.data), ['10.20.0.0/16']);

    changed.data.slice[0].session[0].ipv4_framed_routes = [];
    const cleared = await request('PATCH', '/' + body.imsi, { slice: changed.data.slice });
    assert.equal(cleared.status, 200);
    assert.deepEqual(routesOf(cleared.data), []);
    assert.deepEqual(cleared.data.slice[0].session[1].ipv4_framed_routes, ['172.16.0.0/12']);
    assert.deepEqual(cleared.data.slice[1].session[0].ipv4_framed_routes, ['10.10.0.0/16']);
    assert.deepEqual(routesOf((await request('GET', '/' + body.imsi)).data), []);
    assert.equal((await request('DELETE', '/' + body.imsi)).status, 204);
  });

  test('API reads manually inserted routes and preserves them on unrelated changes', async () => {
    const body = fixture('001010000000003', ['192.168.0.0/16']);
    await Subscriber.collection.insertOne(body);
    const read = await request('GET', '/' + body.imsi);
    assert.equal(read.status, 200);
    assert.deepEqual(routesOf(read.data), ['192.168.0.0/16']);
    const updated = await request('PATCH', '/' + body.imsi, { subscriber_status: 1 });
    assert.equal(updated.status, 200);
    assert.deepEqual(routesOf(updated.data), ['192.168.0.0/16']);
  });

  test('API rejects invalid creates and updates, leaving stored routes unchanged', async () => {
    const imsi = '001010000000004';
    assert.equal((await request('POST', '', fixture(imsi, validRoutes))).status, 201);
    const invalidLists = invalidRoutes.map(route => [route]).concat([
      Array(9).fill('192.168.0.0/16'), null, '192.168.0.0/16'
    ]);
    for (const routes of invalidLists) {
      const created = await request('POST', '', fixture('001010000000005', routes));
      assert.equal(created.status, 400, JSON.stringify(routes));
      const updated = await request('PATCH', '/' + imsi, { slice: fixture(imsi, routes).slice });
      assert.equal(updated.status, 400, JSON.stringify(routes));
      assert.deepEqual(routesOf((await request('GET', '/' + imsi)).data), validRoutes);
    }
    const dotted = await request('PATCH', '/' + imsi, {
      'slice.0.session.0.ipv4_framed_routes': ['192.168.0.0/33']
    });
    assert.equal(dotted.status, 400);
    assert.deepEqual(routesOf((await request('GET', '/' + imsi)).data), validRoutes);
  });

  test('API adds routes to a legacy session with no route field', async () => {
    const imsi = '001010000000006';
    assert.equal((await request('POST', '', fixture(imsi))).status, 201);
    const read = await request('GET', '/' + imsi);
    assert.equal(routesOf(read.data), undefined);
    read.data.slice[0].session[0].ipv4_framed_routes = validRoutes;
    const updated = await request('PATCH', '/' + imsi, { slice: read.data.slice });
    assert.equal(updated.status, 200);
    assert.deepEqual(routesOf(updated.data), validRoutes);
  });
});
