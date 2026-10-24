const express = require('express');
const http2 = require('http2');
const http = require('http');
const crypto = require('crypto');
const mongoose = require('mongoose');
const requireAdmin = require('./core-admin');
const SUPI = /^imsi-[0-9]{5,15}$/;
const VERSION = /^"[0-9a-f-]{36}"$/i;
const failure = (status, message, state) => Object.assign(new Error(message), {status, state});

// Two fixed SBI origins, bounded bodies, deadlines, no proxy and no automatic retry.
function sbiClient() {
  const connections = new Map();
  const request = (origin, path, method = 'GET', body, version) => new Promise((resolve, reject) => {
    let stream, timer, done = false, size = 0, status;
    const chunks = [];
    const finish = (err, value) => {
      if (done) return;
      done = true; clearTimeout(timer);
      if (stream && !stream.closed) stream.close();
      err ? reject(err) : resolve(value);
    };
    try {
      let session = connections.get(origin);
      if (!session || session.closed || session.destroyed) {
        session = http2.connect(origin);
        connections.set(origin, session);
        session.on('error', () => {});
        session.on('close', () => { if (connections.get(origin) === session) connections.delete(origin); });
      }
      stream = session.request({':path': path, ':method': method,
        ...(body ? {'content-type': 'application/json'} : {}), ...(version ? {'if-match': version} : {})});
      timer = setTimeout(() => finish(failure(504, '请求超时，结果待确认；请刷新当前状态，勿重复提交', method === 'GET' ? 'failed' : 'unknown')), 5000);
      stream.on('response', h => { status = h[':status']; });
      stream.on('data', data => {
        size += data.length;
        if (size > 256 * 1024) return finish(failure(502, '核心网响应超过大小限制', method === 'GET' ? 'failed' : 'unknown'));
        chunks.push(data);
      });
      stream.on('error', () => finish(failure(502, '核心网连接中断，请核对当前状态', method === 'GET' ? 'failed' : 'unknown')));
      stream.on('end', () => {
        let data = {};
        try { if (size) data = JSON.parse(Buffer.concat(chunks).toString()); }
        catch (e) { return finish(failure(502, '核心网返回了无效响应', method === 'GET' ? 'failed' : 'unknown')); }
        if (!status) return finish(failure(502, '核心网未返回状态', 'unknown'));
        finish(null, {status, data});
      });
      stream.on('close', () => { if (!done) finish(failure(502, '核心网连接已关闭，结果待确认', 'unknown')); });
      stream.end(body ? JSON.stringify(body) : undefined);
    } catch (e) { finish(failure(503, '核心网控制服务不可用', method === 'GET' ? 'failed' : 'unknown')); }
  });
  request.close = () => { for (const session of connections.values()) session.destroy(); connections.clear(); };
  return request;
}

function auditStore() {
  let initialized;
  const collection = async () => {
    if (!initialized) initialized = (async () => {
      const db = mongoose.connection.db;
      if (!db) throw new Error('Database not ready');
      try { await db.createCollection('core_control_audit', {capped: true, size: 16 * 1024 * 1024, max: 10000}); }
      catch (e) { if (e.code !== 48) throw e; }
      return db.collection('core_control_audit');
    })().catch(e => { initialized = null; throw e; });
    return initialized;
  };
  return {
    async claim(id, record) {
      const c = await collection();
      try { await c.insertOne({_id: 'request:' + id, ...record}); return null; }
      catch (e) {
        if (e.code !== 11000) throw e;
        const old = await c.findOne({_id: 'request:' + id});
        if (!old || old.fingerprint !== record.fingerprint) throw failure(409, '操作编号已被其他请求使用');
        return await c.findOne({_id: 'result:' + id}) || {status: 409, data: {state: 'unknown', error: '该请求已提交，请核对当前状态'}};
      }
    },
    async result(id, record) { await (await collection()).insertOne({_id: 'result:' + id, ...record}); },
    async list(supi) {
      return (await collection()).find({kind: 'result', ...(supi ? {supi} : {})})
        .sort({$natural: -1}).limit(20).project({_id: 0, fingerprint: 0}).toArray();
    }
  };
}

function monitorUsers() {
  const base = process.env.MONITOR_URL;
  const read = page => new Promise((resolve, reject) => {
    if (!base) return reject(failure(503, '监控服务未配置'));
    const req = http.get(new URL('/api/ues?limit=100&page=' + page, base),
      {headers: {Authorization: 'Bearer ' + process.env.MONITOR_INTERNAL_TOKEN}}, res => {
        const parts = []; let size = 0;
        res.on('data', b => { size += b.length; if (size > 1024 * 1024) res.destroy(new Error('limit')); else parts.push(b); });
        res.on('error', reject);
        res.on('end', () => {
          try {
            const data = JSON.parse(Buffer.concat(parts).toString());
            if (res.statusCode !== 200 || !data.sources || !data.sources.amf_ues || data.sources.amf_ues.stale)
              throw failure(503, '终端数据未更新，暂不能生成释放清单');
            resolve(data);
          } catch (e) { reject(e); }
        });
      });
    const deadline = setTimeout(() => req.destroy(new Error('timeout')), 3000);
    req.on('close', () => clearTimeout(deadline)); req.on('error', reject);
  });
  return (async () => {
    const first = await read(0);
    if (first.total > 1024) throw failure(409, '用户超过单次批量操作上限 1024，请分批处理');
    const supis = first.items.map(r => r.supi);
    for (let page = 1; page * 100 < first.total; page++) supis.push(...(await read(page)).items.map(r => r.supi));
    return [...new Set(supis.filter(s => SUPI.test(s)))];
  })();
}

function validateRule(body) {
  if (!body || Object.keys(body).some(k => !['qos', 'flowDescriptions'].includes(k))) throw failure(400, '规则包含不支持的字段');
  if (!Array.isArray(body.flowDescriptions) || !body.flowDescriptions.length || body.flowDescriptions.length > 15 ||
      body.flowDescriptions.some(v => typeof v !== 'string' || v.length > 2048 || !/^permit (in|out) ip from .+ to .+$/.test(v)))
    throw failure(400, '请填写 1–15 条有效的上下行过滤规则');
  const q = body.qos;
  if (!q || !Number.isInteger(q['5qi']) || q['5qi'] < 1 || q['5qi'] > 255) throw failure(400, '5QI 必须为 1–255 的整数');
  const keys = ['5qi', 'priorityLevel', 'arp', 'precedence', 'maxbrUl', 'maxbrDl', 'gbrUl', 'gbrDl'];
  if (Object.keys(q).some(k => !keys.includes(k))) throw failure(400, 'QoS 包含不支持的字段');
  for (const [k, lo, hi] of [['priorityLevel', 1, 127], ['precedence', 0, 254]])
    if (q[k] !== undefined && (!Number.isInteger(q[k]) || q[k] < lo || q[k] > hi)) throw failure(400, k + ' 超出有效范围');
  if (q.arp && (!Number.isInteger(q.arp.priorityLevel) || q.arp.priorityLevel < 1 || q.arp.priorityLevel > 15 ||
      !['NOT_PREEMPT', 'MAY_PREEMPT'].includes(q.arp.preemptionCapability) ||
      !['NOT_PREEMPTABLE', 'PREEMPTABLE'].includes(q.arp.preemptionVulnerability) ||
      Object.keys(q.arp).some(k => !['priorityLevel', 'preemptionCapability', 'preemptionVulnerability'].includes(k))))
    throw failure(400, 'ARP 参数无效');
  for (const k of ['maxbrUl', 'maxbrDl', 'gbrUl', 'gbrDl']) if (q[k] !== undefined &&
      (typeof q[k] !== 'string' || q[k].length > 40 || !/^\d+(\.\d+)? (bps|Kbps|Mbps|Gbps|Tbps)$/.test(q[k])))
    throw failure(400, '速率格式示例：10 Mbps');
}

module.exports = function controlRouter(options = {}) {
  const router = express.Router();
  const send = options.send || sbiClient(), audit = options.audit || auditStore();
  const pcf = options.pcf || process.env.CORE_CONTROL_PCF_URL;
  const amf = options.amf || process.env.CORE_CONTROL_AMF_URL;
  const users = options.users || monitorUsers;
  let active = 0, reads = 0, tokens = 2, lastToken = Date.now();
  const now = options.now || Date.now;
  const locked = new Set();
  router.use(options.admin || requireAdmin());
  router.use((req, res, next) => {
    res.set('Cache-Control', 'no-store');
    if (!pcf || !amf) return res.status(503).json({error: '核心网控制服务未配置'});
    if (req.body && Buffer.byteLength(JSON.stringify(req.body)) > 32768) return res.status(413).json({error: '请求内容过大'});
    next();
  });
  const route = fn => async (req, res) => {
    try { await fn(req, res); }
    catch (e) { res.status(e.status || 503).json({state: e.state || 'failed', error: e.status ? e.message : '控制服务暂时不可用'}); }
  };
  const read = fn => route(async (req, res) => {
    if (reads >= 16) throw failure(429, '查询繁忙，请稍后重试');
    reads++; try { res.json(await fn(req)); } finally { reads--; }
  });
  const checked = result => {
    if (result.status < 200 || result.status >= 300) {
      const message = ({404: '目标已不存在，请刷新', 409: '目标正在处理其他操作或不支持此修改，请刷新后检查',
        412: '会话或策略已变化，请刷新后重新编辑', 428: '缺少会话版本，请重新读取详情'})[result.status];
      throw failure(result.status, message || String(result.data.detail || result.data.title || '核心网拒绝了请求').slice(0, 300));
    }
    return result.data;
  };
  const identity = req => {
    if (!SUPI.test(req.params.supi) || !/^(?:[1-9]|1[0-5])$/.test(req.params.psi)) throw failure(400, '终端或会话标识无效');
    return req.params;
  };
  const version = req => {
    const v = req.get('If-Match'); if (!VERSION.test(v || '')) throw failure(428, '请先读取当前会话，再提交操作'); return v;
  };
  const bearerPath = req => '/xcn-dedicated-bearer/v1/bearers?supi=' + req.params.supi + '&pduSessionId=' + req.params.psi;
  const sessionPath = req => '/xcn-session-control/v1/sessions/' + req.params.supi + '/' + req.params.psi;
  const write = (action, fn) => route(async (req, res) => {
    const supi = req.params.supi;
    if (!SUPI.test(supi)) throw failure(400, '终端标识无效');
    const id = req.get('X-Operation-ID');
    if (!/^[a-zA-Z0-9-]{16,80}$/.test(id || '')) throw failure(400, '缺少有效操作编号');
    tokens = Math.min(2, tokens + Math.max(0, now() - lastToken) / 500);
    lastToken = now();
    if (active >= 2 || locked.has(supi) || tokens < 1) throw failure(429, '操作处理中，请稍后核对状态');
    tokens--; active++; locked.add(supi);
    try {
      const fingerprint = crypto.createHash('sha256').update(JSON.stringify([req.controlActor, req.method, req.originalUrl, req.get('If-Match'), req.body])).digest('hex');
      const record = {operationId: id, actor: req.controlActor, supi, psi: req.params.psi, appSessionId: req.params.app,
        action, timestamp: new Date(), fingerprint, kind: 'request', summary: JSON.stringify(req.body || {}).slice(0, 2048)};
      const old = await audit.claim(id, record);
      if (old) return res.status(old.status).json(old.data);
      let result;
      try {
        const upstream = await fn(req);
        const data = checked(upstream);
        result = {status: 202, data: {operationId: id, state: 'accepted', message: '核心网已受理，请查看当前状态及相关信令',
          upstreamStatus: upstream.status, ...(data.appSessionId ? {appSessionId: data.appSessionId} : {}),
          ...(data.accepted !== undefined ? {accepted: data.accepted, skipped: data.skipped} : {})}};
      } catch (e) {
        result = {status: e.status || 503, data: {operationId: id, state: e.state || (e.status >= 500 ? 'unknown' : 'failed'), error: e.status ? e.message : '控制服务暂时不可用'}};
      }
      try { await audit.result(id, {...record, ...result, kind: 'result', timestamp: new Date()}); }
      catch (e) { result.data.auditWarning = '操作记录保存失败，请保留本次结果'; }
      res.status(result.status).json(result.data);
    } finally { active--; locked.delete(supi); }
  });
  router.get('/ues', read(async () => ({supis: await users()})));
  router.get('/operations', read(req => {
    if (req.query.supi && !SUPI.test(req.query.supi)) throw failure(400, '终端标识无效');
    return audit.list(req.query.supi);
  }));
  router.get('/sessions/:supi/:psi', read(async req => {
    identity(req);
    const target = checked(await send(pcf, sessionPath(req)));
    const rules = checked(await send(pcf, bearerPath(req)));
    if (target.controlVersion !== rules.controlVersion) throw failure(409, '会话正在变化，请重新读取');
    return {...target, bearers: rules.bearers};
  }));
  router.post('/sessions/:supi/:psi/bearers', write('create-bearer', async req => {
    identity(req); const v = version(req); validateRule(req.body);
    return send(pcf, '/xcn-dedicated-bearer/v1/bearers', 'POST', {...req.body, supi: req.params.supi, pduSessionId: Number(req.params.psi)}, v);
  }));
  for (const method of ['patch', 'delete']) router[method]('/sessions/:supi/:psi/bearers/:app', write(method === 'patch' ? 'modify-bearer' : 'delete-bearer', async req => {
    identity(req); const v = version(req);
    if (!/^[0-9]{1,10}$/.test(req.params.app)) throw failure(400, '专载标识无效');
    if (method === 'patch') validateRule(req.body);
    const rules = checked(await send(pcf, bearerPath(req)));
    if ('"' + rules.controlVersion + '"' !== v) throw failure(412, '会话或策略已变化，请重新读取');
    const app = rules.bearers.find(b => b.appSessionId === req.params.app);
    if (!app) throw failure(404, '该专载不属于当前会话');
    if (app.intentManaged || app.deletePending) throw failure(409, '该专载由终端意图管理或正在删除');
    return send(pcf, '/xcn-dedicated-bearer/v1/bearers/' + req.params.app, method.toUpperCase(), method === 'patch' ? req.body : undefined, v);
  }));
  router.delete('/sessions/:supi/:psi', write('release-session', req => {
    identity(req); return send(pcf, sessionPath(req), 'DELETE', undefined, version(req));
  }));
  router.delete('/ues/:supi', write('release-ue', req => send(amf, '/namf-oam/v1/ues/' + req.params.supi, 'DELETE')));
  router.use((req, res) => res.status(405).json({error: '不支持此控制操作'}));
  router.close = () => { if (send.close) send.close(); };
  return router;
};
module.exports.sbiClient = sbiClient;
module.exports.validateRule = validateRule;
