// Read-only monitoring bypasses express-session/MongoDB entirely.
const express = require('express');
const http = require('http');
const https = require('https');
const jwt = require('jsonwebtoken');
const ENDPOINTS = new Set(['overview', 'ues', 'gnbs', 'sessions', 'rates', 'events', 'trends', 'health', 'system']);

module.exports = function monitorRouter(options = {}) {
  const router = express.Router();
  const base = options.url || process.env.MONITOR_URL;
  const internalToken = options.token || process.env.MONITOR_INTERNAL_TOKEN;
  const secret = options.secret || process.env.JWT_SECRET_KEY;
  const entries = new Map();
  const pending = new Map();
  let bytes = 0, inflightBytes = 0;
  const agent = new http.Agent({keepAlive: true, maxSockets: 16});
  const secureAgent = new https.Agent({keepAlive: true, maxSockets: 16});

  function fetch(path) {
    if (pending.has(path)) return pending.get(path);
    if (pending.size >= 32) return Promise.reject(new Error('monitor busy'));
    let deadline, buffered = 0;
    const promise = new Promise((resolve, reject) => {
      const url = new URL('/api/' + path, base);
      const transport = url.protocol === 'https:' ? https : http;
      const req = transport.get(url, {
        agent: url.protocol === 'https:' ? secureAgent : agent,
        headers: {Authorization: 'Bearer ' + internalToken}
      }, upstream => {
        const chunks = [];
        let size = 0;
        upstream.on('data', chunk => {
          size += chunk.length;
          if (size > 4 * 1024 * 1024 || inflightBytes + chunk.length > 16 * 1024 * 1024) {
            upstream.destroy(new Error('monitor response memory budget exceeded'));
          } else {
            chunks.push(chunk);
            buffered += chunk.length;
            inflightBytes += chunk.length;
          }
        });
        upstream.on('error', reject);
        upstream.on('end', () => {
          if (upstream.statusCode !== 200) {
            const error = new Error('monitor unavailable');
            error.status = upstream.statusCode === 400 ? 400 : 503;
            return reject(error);
          }
          const entry = {body: Buffer.concat(chunks), etag: upstream.headers.etag, until: Date.now() + 1000};
          while (entries.size && (entries.size >= 64 || bytes + size > 16 * 1024 * 1024)) {
            const first = entries.keys().next().value;
            bytes -= entries.get(first).body.length;
            entries.delete(first);
          }
          if (entries.has(path)) bytes -= entries.get(path).body.length;
          entries.set(path, entry);
          bytes += entry.body.length;
          resolve(entry);
        });
      });
      req.setTimeout(1500, () => req.destroy(new Error('monitor timeout')));
      deadline = setTimeout(() => req.destroy(new Error('monitor deadline exceeded')), 1800);
      req.on('error', reject);
    }).finally(() => { clearTimeout(deadline); inflightBytes -= buffered; pending.delete(path); });
    pending.set(path, promise);
    return promise;
  }

  router.use((req, res, next) => {
    res.set('Cache-Control', 'private, no-cache');
    if (req.method !== 'GET') return res.status(405).json({error: '只读接口'});
    try {
      const match = /^Bearer (\S+)$/.exec(req.headers.authorization || '');
      if (!match) throw new Error('missing token');
      const claims = jwt.verify(match[1], secret, {algorithms: ['HS256'], audience: 'xcn-monitor', issuer: 'xcn-webui'});
      if (claims.scope !== 'monitor:read' || !claims.sub) throw new Error('invalid scope');
    } catch (err) {
      return res.status(401).json({error: '登录已过期，请重新登录'});
    }
    next();
  });

  router.get('/:endpoint', async (req, res) => {
    if (!ENDPOINTS.has(req.params.endpoint)) return res.status(404).end();
    if (!base || !internalToken) return res.status(503).json({error: '监控采集服务尚未启用'});
    if (req.originalUrl.length > 1200 || Object.values(req.query).some(v => typeof v !== 'string')) {
      return res.status(400).json({error: '查询参数无效'});
    }
    const query = new URLSearchParams(Object.keys(req.query).sort().map(k => [k, req.query[k]])).toString();
    const path = req.params.endpoint + (query ? '?' + query : '');
    try {
      const cached = entries.get(path);
      const entry = cached && cached.until > Date.now() ? cached : await fetch(path);
      if (entry.etag) res.set('ETag', entry.etag);
      if (entry.etag && req.headers['if-none-match'] === entry.etag) return res.status(304).end();
      res.type('application/json').send(entry.body);
    } catch (err) {
      res.status(err.status || 503).json({error: err.status === 400 ? '查询参数无效' : '采集服务暂时不可用，保留上次已显示的数据'});
    }
  });
  return router;
};
