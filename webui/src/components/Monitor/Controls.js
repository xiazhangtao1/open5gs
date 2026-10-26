import {Component} from 'react';
import axios from 'axios';
import Session from 'modules/auth/session';
import sessionConfig from '../../../lib/session-control';

const labels = {'create-bearer': '新建专载', 'modify-bearer': '修改专载', 'delete-bearer': '删除专载', 'modify-session': '修改默认承载', 'release-session': '释放会话', 'release-ue': '释放用户'};
const states = {accepted: '已受理', pending: '信令处理中', confirmed: '信令流程完成', failed: '失败', unknown: '结果待确认', unchanged: '参数未变化', 'not-required': '无需修改'};
const message = e => (e.response && e.response.data && e.response.data.error) || e.message || '请求未完成，请核对当前状态';
const operationId = () => {
  const bytes = new Uint8Array(16); window.crypto.getRandomValues(bytes);
  return Array.from(bytes, n => ('0' + n.toString(16)).slice(-2)).join('');
};
async function control(method, path, data, version) {
  const headers = {};
  if (method !== 'get') {
    headers['X-CSRF-TOKEN'] = await Session.getCsrfToken();
    headers['X-Operation-ID'] = operationId();
    if (version) headers['If-Match'] = '"' + version + '"';
  }
  return (await axios({method, url: '/api/core-control' + path, data, headers, timeout: 12000})).data;
}
const Button = ({children, danger, primary, ...props}) => <button type="button"
  className={'xcn-button' + (danger ? ' xcn-danger' : primary ? ' xcn-primary' : '')} {...props}>{children}</button>;
const Notice = ({result}) => result ? <div className={'xcn-control-notice ' + (result.state || 'failed')} role="status">
  <strong>{states[result.state] || '操作提示'}</strong><span>{result.error || result.message}</span>
  {result.auditWarning && <span>{result.auditWarning}</span>}
  {result.operationId && <small>操作编号 {result.operationId}</small>}</div> : null;
function failed(e) {
  return e.response && e.response.data && e.response.data.state ? e.response.data :
    {state: e.response ? 'failed' : 'unknown', error: message(e)};
}

class History extends Component {
  state = {items: [], error: ''};
  componentDidMount() { this.active = true; this.load(); }
  componentWillUnmount() { this.active = false; }
  componentWillReceiveProps(p) { if (p.revision !== this.props.revision || p.supi !== this.props.supi) this.load(p); }
  load = async (props = this.props) => {
    try {
      const items = await control('get', '/operations?supi=' + encodeURIComponent(props.supi));
      if (this.active) this.setState({items, error: ''});
    } catch (e) { if (this.active) this.setState({error: message(e)}); }
  };
  render() {
    return <details className="xcn-control-history"><summary>近期操作记录</summary>
      {this.state.error && <p>{this.state.error}</p>}
      {!this.state.items.length && !this.state.error && <p>暂无操作记录。</p>}
      {this.state.items.slice(0, 10).map(r => <div key={r.operationId}>
        <time>{new Date(r.timestamp).toLocaleTimeString('zh-CN', {hour12: false})}</time>
        <span>{r.actor} · {labels[r.action] || r.action}{r.psi ? ' · PSI ' + r.psi : ''}</span>
        <strong>{states[(r.data || {}).state] || '待确认'}</strong>
        <small>{(r.data || {}).error || ''}</small></div>)}
    </details>;
  }
}

const freshForm = () => ({fiveQi: '9', priority: '', arp: '8', capability: 'NOT_PREEMPT', vulnerability: 'PREEMPTABLE',
  precedence: '', maxbrUl: '', maxbrDl: '', gbrUl: '', gbrDl: '',
  flows: 'permit out ip from 0.0.0.0/0 to assigned\npermit in ip from assigned to 0.0.0.0/0'});
function appForm(app) {
  const rule = app.pccRules[0], q = rule.qos || {}, arp = q.arp || {};
  const bps = n => n === undefined ? '' : n + ' bps';
  return {fiveQi: String(q['5qi']), priority: q.priorityLevel === undefined ? '' : String(q.priorityLevel),
    arp: String(arp.priorityLevel || 8), capability: arp.preemptionCapability === 2 ? 'MAY_PREEMPT' : 'NOT_PREEMPT',
    vulnerability: arp.preemptionVulnerability === 2 ? 'PREEMPTABLE' : 'NOT_PREEMPTABLE', precedence: String(rule.precedence),
    maxbrUl: bps(q.mbrUl), maxbrDl: bps(q.mbrDl), gbrUl: bps(q.gbrUl), gbrDl: bps(q.gbrDl),
    flows: (rule.flows || []).map(f => f.description).join('\n')};
}
function ruleBody(f) {
  const qos = {'5qi': Number(f.fiveQi), arp: {priorityLevel: Number(f.arp),
    preemptionCapability: f.capability, preemptionVulnerability: f.vulnerability}};
  if (f.priority !== '') qos.priorityLevel = Number(f.priority);
  if (f.precedence !== '') qos.precedence = Number(f.precedence);
  ['maxbrUl', 'maxbrDl', 'gbrUl', 'gbrDl'].forEach(k => { if (f[k].trim()) qos[k] = f[k].trim(); });
  return {qos, flowDescriptions: f.flows.split('\n').map(s => s.trim()).filter(Boolean)};
}

const rate = text => {
  if (!text) return '—';
  const match = /^(\d+) bps$/.exec(text);
  if (!match) return text;
  const n = Number(match[1]);
  for (const [unit, divisor] of [['Tbps', 1e12], ['Gbps', 1e9], ['Mbps', 1e6], ['Kbps', 1e3]])
    if (n >= divisor && n % divisor === 0) return n / divisor + ' ' + unit;
  return text;
};
const configRows = value => {
  const c = value || {}, q = c.defaultQos || {}, a = q.arp || {}, r = c.sessionAmbr || {};
  return [['默认 5QI', q['5qi'] || '—'], ['调度优先级', q.priorityLevel || '使用 5QI 默认值'],
    ['ARP 优先级', a.priorityLevel || '—'], ['抢占能力', a.preemptionCapability === 'MAY_PREEMPT' ? '允许抢占' : '不抢占'],
    ['被抢占能力', a.preemptionVulnerability === 'PREEMPTABLE' ? '允许被抢占' : '不允许被抢占'],
    ['Session-AMBR 上行', rate(r.uplink)], ['Session-AMBR 下行', rate(r.downlink)]];
};
function SessionConfig({config}) {
  const rows = configRows(config);
  return <div className="xcn-policy-groups">
    <div className="xcn-policy-qos"><h4>QoS 参数</h4>
      <dl className="xcn-session-config">{rows.slice(0, 5).map(([label, value]) =>
        <div key={label}><dt>{label}</dt><dd>{value}</dd></div>)}</dl></div>
    <div className="xcn-policy-ambr"><h4>Session-AMBR</h4><p>全部 Non-GBR 流的聚合速率</p>
      <dl className="xcn-session-config">{rows.slice(5).map(([label, value], i) =>
        <div key={label}><dt>{i === 0 ? '上行' : '下行'}</dt><dd>{value}</dd></div>)}</dl></div>
  </div>;
}
function sessionForm(config) {
  const q = config.defaultQos, a = q.arp;
  return {kind: 'session', fiveQi: String(q['5qi']), priority: q.priorityLevel === undefined ? '' : String(q.priorityLevel),
    arp: String(a.priorityLevel), capability: a.preemptionCapability, vulnerability: a.preemptionVulnerability,
    uplink: rate(config.sessionAmbr.uplink), downlink: rate(config.sessionAmbr.downlink)};
}
function sessionBody(f) {
  const defaultQos = {'5qi': Number(f.fiveQi), arp: {priorityLevel: Number(f.arp),
    preemptionCapability: f.capability, preemptionVulnerability: f.vulnerability}};
  if (f.priority !== '') defaultQos.priorityLevel = Number(f.priority);
  return sessionConfig.validateSessionConfig({defaultQos, sessionAmbr: {uplink: f.uplink.trim(), downlink: f.downlink.trim()}});
}

// This component only reads the shared monitoring snapshot. It does not query a NF.
export const SessionPolicyStatus = ({session: s}) => {
  const snapshot = s.session_control;
  if (!snapshot) return null;
  const op = snapshot.operation;
  const current = snapshot.confirmed || snapshot.observed;
  const defaultFlow = (s.qos_flows || []).find(q => q.is_default === true);
  return <section className="xcn-session-policy" aria-label="默认承载">
    <div className="xcn-control-heading"><h3>默认承载</h3><small>默认 QFI {defaultFlow ? defaultFlow.qfi : '—'}</small></div>
    <SessionConfig config={current}/>
    {op && ['pending', 'unknown'].includes(op.state) && <div>
      <p className="xcn-control-hint">上方为最近确认的配置，本次下发目标尚未全部确认。</p>
      <details className="xcn-control-target"><summary>本次下发目标</summary>
        <SessionConfig config={op.target}/>
      </details>
    </div>}
    {op && <div className={'xcn-control-notice ' + op.state} role="status">
      <strong>{states[op.state] || '结果待确认'}</strong>
      <div className="xcn-control-stages">{[['pfcp', 'UPF'], ['nas', '终端'], ['ngap', '基站']].map(([key, label]) =>
        <span key={key}>{label}：{states[(op.stages || {})[key]] || '待确认'}</span>)}</div>
      {op.state === 'unknown' && <span>部分参数可能已下发，请核对信令。该会话暂停新的策略修改，可显式释放后重建。</span>}
      {op.state === 'failed' && <span>修改未提交或被核心网拒绝，请重新读取会话后检查。</span>}
      <small>操作编号 {op.operationId}</small>
    </div>}
    <p className="xcn-control-hint">以上参数仅作用于当前会话。</p>
  </section>;
};

export class SessionControls extends Component {
  state = {open: false, target: null, busy: false, loading: false, error: '', form: null, edit: null, confirm: null, result: null, revision: 0};
  componentDidMount() { this.active = true; }
  componentWillUnmount() { this.active = false; }
  path = () => '/sessions/' + encodeURIComponent(this.props.session.supi) + '/' + this.props.session.psi;
  load = async () => {
    this.setState({loading: true, error: ''});
    try {
      const target = await control('get', this.path());
      if (this.active) this.setState({target, loading: false, readAt: Date.now()});
      return target;
    } catch (e) { if (this.active) this.setState({target: null, loading: false, error: message(e)}); }
  };
  open = () => this.setState({open: true}, this.load);
  field = (key, v) => this.setState({form: {...this.state.form, [key]: v}});
  editSession = async () => {
    const target = await this.load();
    if (this.active && target && target.sessionConfig &&
        !(target.sessionControl && ['pending', 'unknown'].includes(target.sessionControl.state)))
      this.setState({form: sessionForm(target.sessionConfig), edit: null, confirm: null, result: null});
  };
  execute = async (method, suffix, data) => {
    if (this.state.busy) return;
    const version = this.state.target.controlVersion;
    this.setState({busy: true, result: null});
    try {
      const result = await control(method, this.path() + suffix, data, version);
      if (!this.active) return;
      this.setState({result, form: null, edit: null, confirm: null, revision: this.state.revision + 1});
      await this.load();
    } catch (e) {
      const result = failed(e);
      if (this.active) this.setState({result, revision: this.state.revision + 1,
        ...(result.state === 'unknown' || (e.response && [404, 409, 412].includes(e.response.status)) ?
          {target: null, form: null, edit: null, confirm: null, error: '请重新读取会话并核对当前状态后再操作'} : {})});
    } finally { if (this.active) this.setState({busy: false}); }
  };
  editor() {
    const {form: f, edit, busy} = this.state;
    if (!f) return null;
    if (f.kind === 'session') return this.sessionEditor();
    const number = (name, key, min, max, optional) => <label>{name}<input type="number" min={min} max={max} step="1"
      required={!optional} value={f[key]} disabled={busy || (key === 'precedence' && !!edit)}
      placeholder={optional ? '使用默认值' : ''} onChange={e => this.field(key, e.target.value)}/></label>;
    return <form className="xcn-control-editor" onSubmit={e => { e.preventDefault(); this.execute(edit ? 'patch' : 'post', '/bearers' + (edit ? '/' + edit : ''), ruleBody(f)); }}>
      <h4>{edit ? '修改专载 ' + edit : '新建专载'}</h4>
      <div className="xcn-control-grid">
        {number('5QI', 'fiveQi', 1, 255)}{number('调度优先级（1–127）', 'priority', 1, 127, true)}
        {number('ARP 优先级（1–15）', 'arp', 1, 15)}{number('规则匹配顺序（0–254）', 'precedence', 0, 254, true)}
        <label>抢占能力<select disabled={busy} value={f.capability} onChange={e => this.field('capability', e.target.value)}>
          <option value="NOT_PREEMPT">不抢占其他承载</option><option value="MAY_PREEMPT">允许抢占</option></select></label>
        <label>被抢占能力<select disabled={busy} value={f.vulnerability} onChange={e => this.field('vulnerability', e.target.value)}>
          <option value="PREEMPTABLE">允许被抢占</option><option value="NOT_PREEMPTABLE">不允许被抢占</option></select></label>
        {[['最大上行速率', 'maxbrUl'], ['最大下行速率', 'maxbrDl'], ['保证上行速率', 'gbrUl'], ['保证下行速率', 'gbrDl']].map(([name, key]) =>
          <label key={key}>{name}<input value={f[key]} disabled={busy} placeholder="例如 10 Mbps" maxLength="40"
            onChange={e => this.field(key, e.target.value)}/></label>)}
      </div>
      <label>业务过滤规则 · 每行一条<textarea rows="4" required maxLength="30720" disabled={busy} value={f.flows}
        spellCheck="false" onChange={e => this.field('flows', e.target.value)}/></label>
      <p className="xcn-control-hint">GBR 类型需填写保证速率与最大速率。调度优先级留空时使用 5QI 默认值。</p>
      <div className="xcn-control-actions"><button className="xcn-button xcn-primary" type="submit" disabled={busy}>{busy ? '正在提交…' : edit ? '保存修改' : '建立专载'}</button>
        <Button disabled={busy} onClick={() => this.setState({form: null, edit: null})}>取消</Button></div>
    </form>;
  }
  sessionEditor() {
    const {form: f, busy, confirm} = this.state;
    if (confirm) return null;
    return <form className="xcn-control-editor" onSubmit={e => {
      e.preventDefault();
      try { this.setState({confirm: {type: 'session-config', config: sessionBody(f)}, result: null}); }
      catch (error) { this.setState({result: {state: 'failed', error: error.message}}); }
    }}>
      <h4>修改当前会话参数</h4>
      <div className="xcn-control-grid">
        <label>默认 5QI<select value={f.fiveQi} disabled={busy} onChange={e => this.field('fiveQi', e.target.value)}>
          {sessionConfig.nonGbrFiveQis.map(q => <option key={q} value={q}>{q} · Non-GBR</option>)}</select></label>
        <label>调度优先级（1–127，可留空）<input type="number" min="1" max="127" step="1" value={f.priority}
          placeholder="使用 5QI 默认值" disabled={busy} onChange={e => this.field('priority', e.target.value)}/></label>
        <label>ARP 优先级（1–15）<input type="number" min="1" max="15" step="1" required value={f.arp}
          disabled={busy} onChange={e => this.field('arp', e.target.value)}/></label>
        <label>抢占能力<select value={f.capability} disabled={busy} onChange={e => this.field('capability', e.target.value)}>
          <option value="NOT_PREEMPT">不抢占其他承载</option><option value="MAY_PREEMPT">允许抢占</option></select></label>
        <label>被抢占能力<select value={f.vulnerability} disabled={busy} onChange={e => this.field('vulnerability', e.target.value)}>
          <option value="PREEMPTABLE">允许被抢占</option><option value="NOT_PREEMPTABLE">不允许被抢占</option></select></label>
        {[['Session-AMBR 上行', 'uplink'], ['Session-AMBR 下行', 'downlink']].map(([label, key]) => <label key={key}>{label}
          <input value={f[key]} required maxLength="40" placeholder="例如 10 Mbps" disabled={busy} onChange={e => this.field(key, e.target.value)}/></label>)}
      </div>
      <p className="xcn-control-hint">仅修改当前 PDU 会话，不修改签约。调度优先级与 ARP 分别配置。</p>
      <div className="xcn-control-actions"><button type="submit" className="xcn-button xcn-primary" disabled={busy}>核对修改</button>
        <Button disabled={busy} onClick={() => this.setState({form: null, confirm: null})}>取消</Button></div>
    </form>;
  }
  render() {
    const {session: s} = this.props;
    const {open, target, busy, loading, confirm, result, error, form} = this.state;
    const observed = ((s.session_control || {}).operation || {});
    const nativeOperation = (target || {}).sessionControl;
    const operation = target && nativeOperation && observed.instance === target.controlInstance &&
      observed.transaction === nativeOperation.transaction && observed.updatedAtUs >= nativeOperation.updatedAtUs ? observed : nativeOperation;
    const nativeBusy = nativeOperation && ['pending', 'unknown'].includes(nativeOperation.state);
    const policyBusy = nativeBusy || (operation && ['pending', 'unknown'].includes(operation.state));
    if (!s.supi || !s.psi) return null;
    if (!open) return <section className="xcn-controls xcn-controls-closed" aria-label={'PSI ' + s.psi + ' 会话管理'}>
      <div className="xcn-control-heading"><div><h3>会话管理</h3><p className="xcn-control-hint">新建专载、调整默认承载或释放当前会话。</p></div>
        <Button onClick={this.open} aria-expanded="false">管理此会话</Button></div></section>;
    return <section className="xcn-controls" aria-label={'PSI ' + s.psi + ' 会话管理'}>
      <div className="xcn-control-heading"><div><h3>会话管理</h3><small>PSI {s.psi} · {(target || s).dnn} · {(target || {}).ipv4 || s.ue_ip || '—'}</small></div>
        <div className="xcn-control-actions">
          <Button disabled={busy || loading} onClick={() => { this.setState({form: null, edit: null, confirm: null}); this.load(); }}>重新读取</Button>
          <Button disabled={busy || loading || !!form || !!confirm} aria-expanded="true"
            onClick={() => this.setState({open: false})}>收起管理</Button></div></div>
      <Notice result={result}/>
      {loading && <p>正在读取当前策略…</p>}{error && <p className="xcn-control-error" role="alert">{error}</p>}
      {target && <div>
        <div className="xcn-control-actions"><Button primary disabled={busy || loading || !!form || !!confirm || target.releasing || policyBusy}
          onClick={() => this.setState({form: freshForm(), edit: null, result: null})}>新建专载</Button>
          <Button disabled={busy || loading || !!form || !!confirm || target.releasing || policyBusy || !target.sessionConfig || s.cm_state !== 'connected'}
            onClick={this.editSession}>修改默认承载</Button>
          <Button danger disabled={busy || loading || !!form || !!confirm || target.releasing ||
              (nativeOperation && nativeOperation.state === 'pending') || (operation && operation.state === 'pending')}
            onClick={() => this.setState({confirm: {type: 'session'}, result: null})}>{target.releasing ? '会话释放中' : '释放会话'}</Button></div>
        {s.cm_state !== 'connected' && <p className="xcn-control-hint">终端连接后可修改默认承载。</p>}
        {operation && policyBusy && <p className="xcn-control-hint">{nativeBusy && operation.state === 'confirmed' ? '管理侧尚未确认结果，请重新读取后核对' : states[operation.state]}，暂不能提交新的策略修改。</p>}
        <div className="xcn-control-rule-list">{(target.bearers || []).map(app => {
          const rule = (app.pccRules || [])[0] || {}, qos = rule.qos || {};
          const observed = (s.qos_flows || []).find(q => q.pcc_rule_id === rule.pccRuleId);
          const disabled = busy || loading || !!form || !!confirm || target.releasing || policyBusy || app.intentManaged || app.deletePending;
          return <div className="xcn-control-rule" key={app.appSessionId}>
            <div><strong>专载 {app.appSessionId}</strong><span>5QI {qos['5qi']} {observed ? '· QFI ' + observed.qfi : ''}</span>
              <small>{app.intentManaged ? '由终端意图管理' : app.deletePending ? '正在删除' : '规则顺序 ' + rule.precedence}</small></div>
            <div className="xcn-control-actions"><Button disabled={disabled || app.pccRules.length !== 1}
              onClick={() => this.setState({form: appForm(app), edit: app.appSessionId, result: null})}>修改专载</Button>
              <Button danger disabled={disabled} onClick={() => this.setState({confirm: {type: 'bearer', app: app.appSessionId}, result: null})}>删除专载</Button></div>
          </div>;
        })}{!target.bearers.length && <p className="xcn-control-hint">此会话暂无可管理的专载，可在上方新建。</p>}</div>
        {this.editor()}
        {confirm && confirm.type === 'session-config' && <div className="xcn-control-confirm xcn-config-confirm" role="group" aria-label="确认会话参数修改">
          <h4>确认修改 PSI {s.psi} 的参数</h4><p>{s.supi} · {target.dnn} · {target.ipv4 || target.ipv6Prefix || '—'}</p>
          <div className="xcn-config-comparison"><div className="xcn-config-comparison-head"><span>参数</span><span>当前配置</span><span>修改后</span></div>
            {configRows(target.sessionConfig).map(([label, value], index) => <div key={label}><strong>{label}</strong><span>{value}</span><span>{configRows(confirm.config)[index][1]}</span></div>)}</div>
          <p>该修改仅作用于当前会话。提交成功表示已受理，可在默认承载区域查看信令流程结果。</p>
          <div className="xcn-control-actions"><Button primary disabled={busy} onClick={() => this.execute('patch', '', confirm.config)}>{busy ? '正在提交…' : '确认修改'}</Button>
            <Button disabled={busy} onClick={() => this.setState({confirm: null})}>返回编辑</Button></div>
        </div>}
        {confirm && confirm.type !== 'session-config' && <div className="xcn-control-confirm" role="group" aria-label="确认操作">
          <h4>{confirm.type === 'session' ? '确认释放 PSI ' + s.psi : '确认删除专载 ' + confirm.app}</h4>
          <p>{confirm.type === 'session' ? '该会话的全部承载与数据连接将释放，终端注册和其他会话保留。' : '该专载的规则将移除，匹配业务可能回到默认流。'}</p>
          <div className="xcn-control-actions"><Button danger disabled={busy} onClick={() => this.execute('delete', confirm.type === 'session' ? '' : '/bearers/' + confirm.app)}>
            {busy ? '正在提交…' : confirm.type === 'session' ? '确认释放会话' : '确认删除专载'}</Button><Button disabled={busy} onClick={() => this.setState({confirm: null})}>取消</Button></div>
        </div>}
      </div>}
      <History supi={s.supi} revision={this.state.revision}/>
    </section>;
  }
}

export class UserRelease extends Component {
  state = {confirm: false, busy: false, result: null};
  componentDidMount() { this.active = true; }
  componentWillUnmount() { this.active = false; }
  release = async () => {
    if (this.state.busy) return;
    this.setState({busy: true, result: null});
    try { const result = await control('delete', '/ues/' + this.props.supi); if (this.active) this.setState({result, confirm: false}); }
    catch (e) { if (this.active) this.setState({result: failed(e)}); }
    finally { if (this.active) this.setState({busy: false}); }
  };
  render() {
    return <div className="xcn-controls xcn-user-release">
      {!this.state.confirm && <Button danger onClick={() => this.setState({confirm: true, result: null})}>释放用户</Button>}
      {this.state.confirm && <div className="xcn-control-confirm"><h4>确认释放 {this.props.supi}</h4>
        <p>将发起去注册并释放该终端全部会话。终端可能自动重新注册。</p>
        <div className="xcn-control-actions"><Button danger disabled={this.state.busy} onClick={this.release}>{this.state.busy ? '正在提交…' : '确认释放'}</Button>
          <Button disabled={this.state.busy} onClick={() => this.setState({confirm: false})}>取消</Button></div></div>}
      <Notice result={this.state.result}/>
    </div>;
  }
}

export class BulkRelease extends Component {
  state = {open: false, users: [], loading: false, running: false, confirm: '', results: [], error: ''};
  componentDidMount() { this.active = true; }
  componentWillUnmount() { this.active = false; this.stopped = true; }
  prepare = async () => {
    this.setState({open: true, loading: true, confirm: '', results: [], error: ''});
    try { const data = await control('get', '/ues'); if (this.active) this.setState({users: data.supis}); }
    catch (e) { if (this.active) this.setState({users: [], error: message(e)}); }
    finally { if (this.active) this.setState({loading: false}); }
  };
  run = async () => {
    if (this.state.running || this.state.confirm !== '释放') return;
    this.stopped = false;
    const users = this.state.users.slice(), results = [];
    this.setState({running: true, results: []});
    for (const supi of users) {
      if (!this.active || this.stopped) break;
      let result;
      try { result = await control('delete', '/ues/' + supi); } catch (e) { result = failed(e); }
      results.push({supi, ...result});
      if (this.active) this.setState({results: results.slice()});
      await new Promise(resolve => setTimeout(resolve, 1000));
    }
    if (this.active) this.setState({running: false, confirm: ''});
  };
  render() {
    const {open, loading, running, users, results} = this.state;
    return <div className="xcn-bulk-release">
      {!open && <details><summary>用户管理</summary><Button danger onClick={this.prepare}>释放全部用户…</Button></details>}
      {open && <section className="xcn-controls"><div className="xcn-control-heading"><h4>批量释放用户</h4>
        <Button disabled={running} onClick={() => this.setState({open: false})}>关闭</Button></div>
        {loading ? <p>正在读取释放清单…</p> : <div><p>本次清单共 <strong>{users.length}</strong> 个终端，将逐个去注册并释放全部会话。终端可能自动重新注册。</p>
          <details className="xcn-bulk-targets"><summary>查看目标清单</summary>{users.map(s => <div key={s}>{s}</div>)}</details>
          {!running && !results.length && users.length > 0 && <label className="xcn-bulk-confirm">输入“释放”以确认<input value={this.state.confirm} onChange={e => this.setState({confirm: e.target.value})}/></label>}
          <div className="xcn-control-actions">{!running && !results.length && <Button danger disabled={this.state.confirm !== '释放' || !users.length} onClick={this.run}>确认逐个释放</Button>}
            {running && <Button onClick={() => { this.stopped = true; }}>停止后续释放</Button>}</div>
          {running && <p>已提交 {results.length} / {users.length}。关闭页面将停止后续提交，已受理的操作继续执行。</p>}
          {results.length > 0 && <div className="xcn-bulk-results">{results.map(r => <div key={r.supi}><span>{r.supi}</span><strong>{states[r.state]}</strong><small>{r.error}</small></div>)}</div>}
        </div>}{this.state.error && <p className="xcn-control-error">{this.state.error}</p>}</section>}
    </div>;
  }
}
