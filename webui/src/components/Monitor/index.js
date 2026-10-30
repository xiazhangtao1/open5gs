import {Component} from 'react';
import axios from 'axios';
import Chart from './Chart';
import SignalDetails from './SignalDetails';
import {SessionControls, SessionPolicyStatus, UserRelease, BulkRelease} from './Controls';
import Rules, {Purpose, ruleIdentifier} from './Rules';
import {get} from './client';
import {mergeTrend} from '../../../lib/trends';

const titles = {overview: '概览', ues: '终端', gnbs: '基站', sessions: '会话', events: '信令', system: '状态'};
const sourceNames = {amf_ues: '终端', smf_pdus: '会话', amf_gnbs: '基站', upf_rates: '用户面速率',
  amf_events: 'AMF 信令', smf_events: 'SMF 信令', pods: '容器'};
const visibleSource = k => k !== 'usage' && !/_metrics$/.test(k);
const value = v => v === null || v === undefined || v === '' ? '—' : String(v);
const clock = v => v ? new Date(Number(v) * 1000).toLocaleTimeString('zh-CN', {hour12: false}) : '尚未采集';
const dateTime = v => v ? new Date(Number(v) * 1000).toLocaleString('zh-CN', {hour12: false}) : '尚未采集';
const eventClock = v => {
  const stamp = Number(v) / 1000;
  if (!Number.isFinite(stamp) || stamp <= 0) return '—';
  const date = new Date(Math.floor(stamp));
  return date.toLocaleTimeString('zh-CN', {hour12: false}) + '.' + ('00' + date.getMilliseconds()).slice(-3);
};
const rate = v => {
  const n = Number(v || 0);
  return n >= 1e9 ? (n / 1e9).toFixed(2) + ' Gbps' : n >= 1e6 ? (n / 1e6).toFixed(2) + ' Mbps' :
    n >= 1000 ? (n / 1000).toFixed(1) + ' Kbps' : n.toFixed(0) + ' bps';
};
const bad = r => /fail|error|reject|异常|失败/i.test(r || '');
const Status = ({state}) => <span className={'xcn-status ' +
  (/^(connected|ready|completed|accepted|created|removed|success)$/.test(state) ? 'good' :
    bad(state) ? 'bad' : /^(unknown|陈旧|异常|Pending|未建立)$/.test(state) ? 'warn' : 'neutral')}>
  {({connected: '已连接', idle: '空闲', unknown: '未知', ready: '就绪', completed: '已完成',
    accepted: '已接受', created: '已创建', removed: '已删除', rejected: '已拒绝', failed: '失败'})[state] || value(state)}</span>;
const Empty = ({children}) => <div className="xcn-empty">{children || '当前没有匹配的数据。可调整筛选条件或检查数据源状态。'}</div>;
const SessionIP = ({session}) => {
  const addresses = Array.from(new Set([session.ipv4, session.ipv6, session.ue_ip].filter(ip => ip && ip !== '-' && ip !== '—')));
  return addresses.length ? <span className="xcn-session-ips xcn-mono">{addresses.map(ip => <span key={ip}>{ip}</span>)}</span> : <span>—</span>;
};
const rowForTrend = (items = [], field, id) => items.find(r => String(r[field]) === String(id)) || items[0] || null;
const sameSessionInstance = (current, previous) => previous &&
  ['seid', 'supi', 'dnn', 'ue_ip'].every(k => String(current[k]) === String(previous[k])) &&
  !(Number(current.uptime_seconds) < Number(previous.uptime_seconds));
class Raw extends Component {
  state = {expanded: false};
  render() {
    return <details className="xcn-raw" open={this.state.expanded}>
      <summary onClick={e => { e.preventDefault(); this.setState({expanded: !this.state.expanded}); }}>{this.props.label || '查看原始记录（JSON）'}</summary>
      {this.state.expanded && <pre>{JSON.stringify(this.props.data, null, 2)}</pre>}
    </details>;
  }
}
const Button = ({children, onClick, ...rest}) => <button type="button" className="xcn-button" onClick={onClick} {...rest}>{children}</button>;

function Events({items, onSelect, compact}) {
  if (!items || !items.length) return <Empty>暂无匹配信令，可切换协议或调整搜索条件。</Empty>;
  return <div className={'xcn-events-scroll' + (compact ? ' compact' : '')}>
    <ol className="xcn-events">{items.map((e, i) => <li key={e.event_id || i} className={bad(e.result) ? 'problem' : ''}>
      <time title={dateTime(Number(e.timestamp_us) / 1e6)}>{eventClock(e.timestamp_us)}</time>
      <span className="xcn-protocol" title={value(e.direction)}>{value(e.protocol)}<small>{String(e.nf || '').toUpperCase()}</small></span>
      <button type="button" className="xcn-link xcn-event-name" title={e.name} onClick={() => onSelect(e)}>{value(e.name)}</button>
      <span className="xcn-mono xcn-event-supi" title={e.supi}>{e.supi || '身份解析中'}</span>
      <span className="xcn-event-info" title={e.info || e.cause}>{e.info || e.cause || value(e.direction)}</span>
      <Status state={e.result}/>
    </li>)}</ol>
  </div>;
}

class Drawer extends Component {
  componentDidMount() {
    this.before = document.activeElement;
    this.bodyOverflow = document.body.style.overflow;
    document.body.style.overflow = 'hidden';
    this.close.focus();
    document.addEventListener('keydown', this.key);
  }
  componentWillUnmount() {
    document.body.style.overflow = this.bodyOverflow;
    document.removeEventListener('keydown', this.key);
    if (this.before && document.contains(this.before)) this.before.focus();
  }
  key = e => {
    if (e.key === 'Escape') this.props.onClose();
    if (e.key === 'Tab') {
      const list = Array.from(this.panel.querySelectorAll('button, a, input, select, textarea, summary, [tabindex="0"]'))
        .filter(e => !e.disabled && e.getClientRects().length);
      const first = list[0], last = list[list.length - 1];
      if (e.shiftKey && document.activeElement === first) { last.focus(); e.preventDefault(); }
      if (!e.shiftKey && document.activeElement === last) { first.focus(); e.preventDefault(); }
    }
  };
  render() {
    return <div className="xcn-drawer-backdrop" onClick={e => { if (e.target === e.currentTarget) this.props.onClose(); }}>
      <aside className="xcn-drawer" role="dialog" aria-modal="true" aria-labelledby="xcn-detail-title" ref={e => { this.panel = e; }}>
        <header><h2 id="xcn-detail-title">{this.props.title}</h2><button type="button" ref={e => { this.close = e; }}
          className="xcn-button" onClick={this.props.onClose} aria-label="关闭详情">关闭</button></header>
        {this.props.children}
      </aside>
    </div>;
  }
}

export default class Monitor extends Component {
  trendViews = new Map();
  state = {data: null, trend: null, error: null, loading: true, page: 0, q: '', search: '',
    cm_state: '', protocol: 'NGAP', level: 'session', rule_scope: 'business', seconds: 1800, trendSeid: null, trendSupi: null, trendBearer: null, detailQfi: null,
    selected: null, detail: null, identityFilters: {}};
  componentDidMount() {
    this.active = true;
    document.addEventListener('visibilitychange', this.visibility);
    this.poll();
  }
  componentWillReceiveProps(next) {
    if (next.view !== this.props.view) {
      this.stop();
      this.trendViews.clear();
      const filters = this.navigateFilters || {};
      this.navigateFilters = null;
      this.setState({data: null, trend: null, error: null, page: 0, q: '', search: '', cm_state: '', protocol: 'NGAP',
        trendSeid: null, trendSupi: null, trendBearer: null, detailQfi: null, selected: null, detail: null, identityFilters: {}, ...filters}, () => this.schedule(0));
    }
  }
  componentWillUnmount() {
    this.active = false;
    this.stop();
    document.removeEventListener('visibilitychange', this.visibility);
  }
  stop = () => {
    clearTimeout(this.timer);
    this.generation = (this.generation || 0) + 1;
    if (this.cancel) this.cancel.cancel();
  };
  visibility = () => { this.stop(); if (!document.hidden) this.schedule(0); };
  schedule = delay => {
    clearTimeout(this.timer);
    if (this.active && !document.hidden) this.timer = setTimeout(this.poll, delay);
  };
  loadTrend = async (key, seconds, token, bearer) => {
    const name = key + '/' + seconds;
    // A minute response may be reused for 60 seconds, but never across a
    // bearer instance change, release or admission/eviction transition.
    const signature = key.startsWith('bearer:') ?
      (bearer ? [bearer.trend_epoch, bearer.history_state, bearer.history_reason].join('/') : 'absent') : '';
    const cached = this.trendViews.get(name);
    const old = cached && cached.signature === signature ? cached : null;
    if (old && old.nextAt > Date.now()) return old.data;
    const generation = this.generation;
    try {
      const data = await get('trends', {key, seconds, ...(old && old.data.cursor ? {cursor: old.data.cursor} : {})}, token);
      const merged = mergeTrend(old && old.data, data);
      if (generation === this.generation) {
        if (!this.trendViews.has(name) && this.trendViews.size >= 2) this.trendViews.delete(this.trendViews.keys().next().value);
        const currentInstance = !bearer || !bearer.trend_epoch || data.epoch === bearer.trend_epoch;
        // The shared HTTP proxy can return a response up to one second older
        // than the list. Retry promptly instead of caching that old instance.
        if (!currentInstance) return {points: [], error: '承载实例已变化，正在读取新历史'};
        this.trendViews.set(name, {data: merged, signature, nextAt: seconds > 1800 ? Date.now() + 60000 : 0});
      }
      return merged;
    } catch (error) {
      if (axios.isCancel(error)) throw error;
      const data = {...(old && old.data || {}), error: '速率历史暂时不可用，等待恢复'};
      if (generation === this.generation) {
        if (!this.trendViews.has(name) && this.trendViews.size >= 2) this.trendViews.delete(this.trendViews.keys().next().value);
        this.trendViews.set(name, {data, signature, nextAt: Date.now() + (seconds > 1800 ? 60000 : 2000)});
      }
      return data;
    }
  };
  resetSessionTrend = (session, previous) => {
    if (session && !sameSessionInstance(session, previous)) {
      // Minute views can be cached for 60 seconds; never carry them into a reused SEID.
      for (const name of this.trendViews.keys())
        if (name.startsWith('session:' + session.seid + '/')) this.trendViews.delete(name);
    }
  };
  loadDetail = async (selected, seconds, token) => {
    const supi = selected.row.supi;
    const [sessions, events, ues] = await Promise.all([get('sessions', {supi, limit: 100}, token),
      get('events', {supi, limit: 20}, token), get('ues', {supi, limit: 1}, token)]);
    const sessionScope = selected.type === 'sessions' && selected.row.seid;
    const session = sessions.items.find(s => String(s.seid) === String(selected.row.seid) && s.supi === supi);
    if (sessionScope) {
      const previous = ((this.state.detail && this.state.detail[0].items) || []).find(s =>
        String(s.seid) === String(selected.row.seid) && s.supi === supi) || selected.row;
      this.resetSessionTrend(session, previous);
    }
    const qfi = this.state.detailQfi;
    const bearer = session && (session.bearers || []).find(b => b.qfi === qfi);
    const trend = sessionScope && !session && qfi === null ? null : await this.loadTrend(
      sessionScope ? qfi !== null ? 'bearer:' + selected.row.seid + ':' + qfi : 'session:' + session.seid :
        'user:' + supi, seconds, token, bearer);
    return [sessions, events, trend, ues];
  };
  poll = async () => {
    if (!this.active || document.hidden) return;
    const generation = this.generation;
    const {view} = this.props;
    const {q, page, cm_state, protocol, level, seconds, selected, trendSeid, trendSupi, trendBearer} = this.state;
    this.cancel = axios.CancelToken.source();
    const token = this.cancel.token;
    try {
      const params = {q, page, limit: 50, cm_state, ...this.state.identityFilters};
      if (view === 'events' || view === 'overview') params.protocol = protocol;
      const endpoint = view === 'sessions' ? 'rates' : view;
      if (view === 'sessions') params.level = level;
      if (view === 'sessions' && level === 'rule') params.rule_scope = this.state.rule_scope;
      const data = await get(endpoint, params, token);
      if (!this.active || generation !== this.generation) return;
      const session = view === 'sessions' && level === 'session' ? rowForTrend(data.items, 'seid', trendSeid) : null;
      const user = view === 'sessions' && level === 'user' ? rowForTrend(data.items, 'supi', trendSupi) : null;
      const previous = ((this.state.data || {}).items || []).find(s => session && String(s.seid) === String(session.seid));
      this.resetSessionTrend(session, previous);
      const bearer = view === 'sessions' && level === 'bearer' ?
        (trendBearer ? data.items.find(b => b.trend_key === trendBearer.trend_key) : data.items[0]) : null;
      const bearerSelection = bearer || trendBearer;
      const trendKey = view === 'sessions' && level === 'bearer' ? bearerSelection && bearerSelection.trend_key : view === 'overview' ? 'global' : user ? 'user:' + user.supi : session ? 'session:' + session.seid : null;
      const needsDetail = selected && selected.row.supi && selected.type !== 'events';
      const calls = [trendKey ? this.loadTrend(trendKey, seconds, token, bearer) : Promise.resolve(null),
        needsDetail ? this.loadDetail(selected, seconds, token) : Promise.resolve(null)];
      const results = await Promise.all(calls);
      if (!this.active || generation !== this.generation) return;
      this.setState({data, trend: results[0], detail: results[1],
        trendBearer: bearerSelection,
        trendSeid: session ? String(session.seid) : trendSeid,
        trendSupi: user ? user.supi : trendSupi,
        error: null, loading: false, updated: Date.now()});
    } catch (e) {
      if (!this.active || generation !== this.generation || axios.isCancel(e)) return;
      this.setState({error: e.response && e.response.data && e.response.data.error || '监控连接中断，等待恢复。', loading: false});
    } finally {
      if (this.active && generation === this.generation) this.schedule(2000 + Math.random() * 200);
    }
  };
  change = change => {
    this.stop();
    if ('seconds' in change || 'selected' in change) this.trendViews.clear();
    this.setState({...change, ...('seconds' in change ? {trend: null, detail: null} : {}),
      ...('level' in change ? {data: null, trend: null} : {}), loading: true}, () => this.schedule(0));
  };
  search = e => { e.preventDefault(); this.change({q: this.state.search.trim(), page: 0}); };
  select = (row, type) => this.change({selected: {row, type}, detail: null, detailQfi: type === 'sessions' && row.qfi ? row.qfi : null,
    ...(this.props.view === 'sessions' && type === 'sessions' && ['user', 'session', 'bearer'].includes(this.state.level) ?
      this.trendSelection(row) : {})});
  trendSelection = row => this.state.level === 'bearer' ? {trendBearer: row, trend: null} : ({[this.state.level === 'user' ? 'trendSupi' : 'trendSeid']:
    this.state.level === 'user' ? row.supi : String(row.seid), trend: null});
  isTrendSelected = row => this.state.level === 'bearer' ? !!this.state.trendBearer && row.trend_key === this.state.trendBearer.trend_key : this.state.level === 'user' ? row.supi === this.state.trendSupi : String(row.seid) === this.state.trendSeid;
  selectTrend = row => {
    if (!this.isTrendSelected(row)) this.change(this.trendSelection(row));
  };
  close = () => this.change({selected: null, detail: null});
  navigate = (view, filters = {}) => {
    if (view === this.props.view) this.change({...filters, selected: null, page: 0});
    else { this.navigateFilters = filters; this.props.onNavigate(view); }
  };
  windowPicker() {
    return <select aria-label="走势时间范围" value={this.state.seconds}
      onChange={e => this.change({seconds: Number(e.target.value)})}>
      <option value={300}>最近 5 分钟</option><option value={900}>最近 15 分钟</option><option value={1800}>最近 30 分钟</option>
      <option value={43200}>最近 12 小时</option><option value={86400}>最近 24 小时</option>
    </select>;
  }
  protocolPicker(data) {
    const protocols = Array.from(new Set(['NGAP'].concat(data.protocols || [], this.state.protocol || []))).sort();
    return <label className="xcn-protocol-filter">PROTOCOL
      <select aria-label="PROTOCOL" value={this.state.protocol} onChange={e => this.change({protocol: e.target.value, page: 0})}>
        <option value="">全部协议</option>{protocols.map(p => <option key={p} value={p}>{p}</option>)}
      </select></label>;
  }
  overview(data) {
    const states = data.cm_states || {};
    const health = data.health || {};
    const problems = Object.keys(health.sources || {}).filter(k => visibleSource(k) && (health.sources[k].stale || health.sources[k].error));
    return <div>
      <section className="xcn-summary" aria-label="核心网概览">
        <button type="button" className="xcn-summary-primary" onClick={() => this.navigate('ues', {cm_state: 'connected'})}>
          <span>已连接终端</span><strong>{states.connected || 0}</strong>
          <small><i className="xcn-live-dot"/>已注册 {data.ue_count}</small></button>
        <button type="button" onClick={() => this.navigate('ues', {cm_state: 'idle'})}>
          <span>空闲终端</span><strong>{states.idle || 0}</strong>
          {states.unknown > 0 && <small>未知 {states.unknown}</small>}</button>
        <button type="button" onClick={() => this.navigate('gnbs')}><span>基站</span><strong>{data.gnb_count}</strong></button>
        <button type="button" onClick={() => this.navigate('sessions')}><span>PDU 会话</span><strong>{data.session_count}</strong></button>
      </section>
      {problems.length > 0 && <div className="xcn-alert warn" role="status">采集待恢复：{problems.map(k => sourceNames[k] || k).join('、')}
        <button type="button" className="xcn-link" onClick={() => this.navigate('system')}>查看采集状态</button></div>}
      <section className="xcn-panel xcn-traffic-panel"><div className="xcn-panel-heading"><h2>用户面流量</h2>{this.windowPicker()}</div>
        <div className="xcn-flow-totals"><div><span><i className="xcn-flow-dot ul"/>上行</span><strong>{rate(data.ul_bps)}</strong></div>
          <div><span><i className="xcn-flow-dot dl"/>下行</span><strong>{rate(data.dl_bps)}</strong></div></div>
        <Chart theme={this.props.theme} trend={this.state.trend}/>
      </section>
      <section className="xcn-panel"><div className="xcn-panel-heading"><h2>近期信令</h2>
        <div className="xcn-panel-actions">{this.protocolPicker(data)}<Button onClick={() => this.navigate('events', {protocol: this.state.protocol})}>更多信令</Button></div></div>
        <Events items={data.events} onSelect={e => this.select(e, 'events')}/></section>
    </div>;
  }
  columns(view) {
    if (view === 'ues') return [['终端 SUPI', r => <button className="xcn-link xcn-mono" type="button" onClick={() => this.select(r, view)}>{value(r.supi)}</button>],
      ['连接状态', r => <Status state={r.cm_state}/>], ['接入基站', r => {
        const g = r.gnb && r.gnb.gnb_id !== null ? r.gnb : (r.location || {}).gnb || {};
        return <button type="button" className="xcn-link xcn-mono" disabled={g.gnb_id === undefined || g.gnb_id === null}
          onClick={() => this.navigate('gnbs', {identityFilters: {gnb_id: String(g.gnb_id), plmn: g.plmn, gnb_id_bits: String(g.gnb_id_bits)}})}>{value(g.gnb_id)}</button>;
      }], ['PDU 会话', r => (r.pdu_sessions || []).length], ['上行 / 下行', r => <span className="xcn-nowrap">{rate(r.ul_bps)} / {rate(r.dl_bps)}</span>],
      ['小区 NCI', r => <span className="xcn-mono">{value(((r.location || {}).nr_cgi || {}).nci_hex)}</span>]];
    if (view === 'gnbs') return [['gNB ID', r => <button type="button" className="xcn-link xcn-mono" onClick={() => this.select(r, view)}>{value(r.gnb_id)}</button>],
      ['PLMN', r => value(r.plmn)], ['ID 位数', r => value(r.gnb_id_bits)],
      ['NG 接口', r => <Status state={(r.ng || {}).setup_success ? 'ready' : '未建立'}/>],
      ['SCTP 对端', r => <span className="xcn-mono">{value((((r.ng || {}).sctp || {}).peer))}</span>],
      ['关联终端', r => (r.associated_ues || []).length], ['观测小区', r => (r.observed_cells || []).length]];
    const base = [['终端 SUPI', r => <button className="xcn-link xcn-mono xcn-nowrap" type="button"
      onClick={e => { e.stopPropagation(); this.select(r, 'sessions'); }}>{value(r.supi)}</button>],
      ['连接状态', r => <Status state={r.cm_state}/>]];
    if (['user', 'session', 'bearer'].includes(this.state.level)) base.unshift(['走势', r => <label className="xcn-trend-choice"
      title={'查看此' + (this.state.level === 'user' ? '用户' : this.state.level === 'bearer' ? '承载' : '会话') + '的流量走势'}
      onClick={e => e.stopPropagation()}><input type="radio" name={'trend-' + this.state.level}
        aria-label={'查看 ' + r.supi + (this.state.level === 'user' ? '' : ' PSI ' + value(r.psi) + (this.state.level === 'bearer' ? ' QFI ' + r.qfi : '')) + ' 的流量走势'}
        checked={this.isTrendSelected(r)} onChange={() => this.selectTrend(r)}/></label>]);
    if (this.state.level !== 'user') base.push(['PSI / SEID', r => <span className="xcn-mono xcn-nowrap">{value(r.psi)} / {value(r.seid)}</span>]);
    else base.push(['会话数', r => value(r.session_count)]);
    const addresses = this.state.level === 'user' ? [] : [['会话 IP', r => <SessionIP session={r}/>]];
    if (this.state.level === 'bearer' || this.state.level === 'rule') base.push(['QFI / 5QI', r => ruleIdentifier(r.qfi) + ' / ' + ruleIdentifier(r['5qi'])]);
    if (this.state.level === 'rule') return base.concat(addresses, [['用途', r => <Purpose rule={r}/>], ['PDR / QER', r => ruleIdentifier(r.pdr_id) + ' / ' + ruleIdentifier(r.qer_id)],
      ['方向', r => r.direction], ['速率', r => <span className="xcn-nowrap">{rate(r.bps)}</span>], ['累计字节', r => <span className="xcn-mono">{value(r.bytes)}</span>]]);
    return base.concat([['DNN', r => value(r.dnn)]], addresses, [['上行', r => <span className="xcn-nowrap">{rate(r.ul_bps)}</span>], ['下行', r => <span className="xcn-nowrap">{rate(r.dl_bps)}</span>],
      ['累计字节 ↑ / ↓', r => <span className="xcn-mono xcn-nowrap">{value(r.ul_bytes)} / {value(r.dl_bytes)}</span>]]);
  }
  table(view, data) {
    const columns = this.columns(view);
    const selectable = view === 'sessions' && ['user', 'session', 'bearer'].includes(this.state.level);
    if (!data.items || !data.items.length) return <Empty/>;
    return <div className="xcn-table-scroll"><table className={'xcn-table' + (view === 'sessions' ? ' xcn-session-table' : '')}><thead><tr>{columns.map(([title]) => <th key={title} scope="col">{title}</th>)}</tr></thead>
      <tbody>{data.items.map((row, i) => <tr key={row.event_id || [row.supi, row.seid, row.qfi, row.pdr_id, row.gnb_id, i].join('/')}
        className={selectable ? 'xcn-trend-row' + (this.isTrendSelected(row) ? ' selected' : '') : undefined}
        onClick={selectable ? () => this.selectTrend(row) : undefined}>
        {columns.map(([title, render]) => <td key={title}>{render(row)}</td>)}</tr>)}</tbody></table></div>;
  }
  filters() {
    const {view} = this.props;
    return <div className="xcn-toolbar">
      <form onSubmit={this.search}><label className="xcn-sr-only" htmlFor="monitor-search">搜索监控数据</label>
        <input id="monitor-search" placeholder="搜索 SUPI、IP、DNN、标识…" value={this.state.search}
          onChange={e => this.setState({search: e.target.value})}/><Button onClick={this.search}>搜索</Button></form>
      {(view === 'ues' || view === 'sessions') && <select aria-label="连接状态筛选" value={this.state.cm_state}
        onChange={e => this.change({cm_state: e.target.value, page: 0})}>
        <option value="">全部连接状态</option><option value="connected">已连接</option><option value="idle">空闲</option><option value="unknown">未知</option></select>}
      {view === 'events' && this.protocolPicker(this.state.data || {})}
      {(this.state.q || Object.keys(this.state.identityFilters).length > 0) && <Button onClick={() => this.change({q: '', search: '', page: 0, identityFilters: {}})}>清除筛选</Button>}
    </div>;
  }
  system(data) {
    const h = data.health || {}, sources = h.sources || {}, memory = h.memory || {};
    const mib = bytes => bytes === undefined ? '—' : (bytes / 1048576).toFixed(1) + ' MiB';
    const used = memory.limit_bytes ? Math.min(100, 100 * memory.used_bytes / memory.limit_bytes) : 0;
    return <div>
      {h.affinity && !h.affinity.verified && <div className="xcn-alert warn">{h.affinity.error || 'CPU 隔离尚未验证'}</div>}
      <section className="xcn-panel"><div className="xcn-panel-heading"><h2>采集服务</h2>
        <Status state={memory.paused ? '内存保护中' : h.enabled ? 'ready' : '已暂停'}/></div>
        <div className="xcn-memory"><div><span>监控服务内存</span><strong>{mib(memory.used_bytes)}<small> / {mib(memory.limit_bytes)}</small></strong></div>
          <progress value={used} max={100} aria-label="监控服务内存使用率"/>
          <span>启动于 {dateTime(h.started_at)}</span></div>
        {memory.last_reason && <p className="xcn-panel-note">最近保护原因：{memory.last_reason}</p>}</section>
      <section className="xcn-panel"><div className="xcn-panel-heading"><h2>数据源</h2></div>
        <div className="xcn-table-scroll"><table className="xcn-table"><thead><tr><th>数据</th><th>状态</th><th>最近更新</th><th>更新周期</th><th>说明</th></tr></thead>
          <tbody>{Object.keys(sources).filter(visibleSource).map(k => <tr key={k}><td>{sourceNames[k] || k}</td>
            <td><Status state={sources[k].error ? '异常' : sources[k].stale ? '等待更新' : 'ready'}/></td>
            <td className="xcn-nowrap">{clock(sources[k].updated_at)}</td><td>{sources[k].interval} 秒</td>
            <td className="xcn-wrap">{sources[k].error || (sources[k].gap_count ? '检测到信令缺口 ' + sources[k].gap_count + ' 次' :
              sources[k].reset_count ? '网元序列重置 ' + sources[k].reset_count + ' 次' : '—')}</td></tr>)}</tbody></table></div>
      </section>
      <section className="xcn-panel"><div className="xcn-panel-heading"><h2>容器状态</h2></div>
        {!data.pods || !data.pods.length ? <Empty>尚未获取容器状态。</Empty> : <div className="xcn-table-scroll"><table className="xcn-table">
          <thead><tr><th>容器 / Pod</th><th>节点</th><th>状态</th><th>重启次数</th></tr></thead>
          <tbody>{data.pods.map(p => (p.containers || []).map(c => <tr key={p.name + c.name}>
            <td><span className="xcn-mono">{c.name}</span><small className="xcn-subtext">{p.name}</small></td>
            <td>{p.node}</td><td><Status state={c.ready ? 'ready' : p.phase}/></td><td>{c.restarts}</td>
          </tr>))}</tbody></table></div>}</section>
    </div>;
  }
  canControl() {
    return (((this.props.session || {}).user || {}).roles || []).indexOf('admin') >= 0;
  }
  trafficTrend(data) {
    const user = this.state.level === 'user', bearer = this.state.level === 'bearer';
    const label = user ? '用户' : bearer ? '承载' : '会话';
    const s = bearer ? this.state.trendBearer : rowForTrend(data.items, user ? 'supi' : 'seid', user ? this.state.trendSupi : this.state.trendSeid);
    const current = !bearer || data.items.some(b => s && b.trend_key === s.trend_key);
    return <section className="xcn-panel xcn-session-trend" aria-label={label + '流量走势'}
      data-qfi={bearer && s ? s.qfi : ''} data-seid={s && !user ? s.seid : ''} data-supi={s ? s.supi : ''}>
      <div className="xcn-panel-heading"><div><h2>{label}流量走势</h2>
        <p>{user ? '按 SUPI 汇总该用户全部 PDU 会话的上行与下行历史' : bearer ? '所选 QoS 流的上行与下行历史' : '所选 PDU 会话的上行与下行历史'}</p></div>
        {this.windowPicker()}</div>
      {s ? <div>
        <div className="xcn-trend-context"><div className="xcn-trend-identity">
          <button type="button" className="xcn-link xcn-mono" onClick={() => this.select(s, 'sessions')}>{value(s.supi)}</button>
          {user ? <span>当前 {value(s.session_count)} 条会话<span>DNN {value(s.dnn)}</span></span> :
            <span>PSI {value(s.psi)}<span>SEID {value(s.seid)}</span><span>{value(s.dnn)}</span>{bearer && <span>QFI {value(s.qfi)} / 5QI {value(s['5qi'])}</span>}</span>}
        </div>{!user && <SessionIP session={s}/>}
          <div className="xcn-trend-rates"><div><span><i className="xcn-flow-dot ul"/>当前上行</span><strong>{current ? rate(s.ul_bps) : '—'}</strong></div>
            <div><span><i className="xcn-flow-dot dl"/>当前下行</span><strong>{current ? rate(s.dl_bps) : '—'}</strong></div></div>
        </div>
        <Chart theme={this.props.theme} trend={this.state.trend}/>
      </div> : <Empty>暂无匹配的{label}，可调整筛选条件。</Empty>}
    </section>;
  }
  sessionRules(s, trend) {
    const qfi = this.state.detailQfi;
    return <section className="xcn-session-detail" key={s.seid} aria-label={'PSI ' + value(s.psi) + ' 会话信息'}>
      <div className="xcn-session-heading"><h3>会话信息</h3><span>PSI {value(s.psi)}</span><span className="xcn-dnn">{value(s.dnn)}</span>
        <span className="xcn-mono">SEID {value(s.seid)}</span></div>
      <dl className="xcn-session-overview"><div><dt>会话 IP</dt><dd><SessionIP session={s}/></dd></div>
        <div><dt><i className="xcn-flow-dot ul"/>当前上行</dt><dd>{rate(s.ul_bps)}</dd></div>
        <div><dt><i className="xcn-flow-dot dl"/>当前下行</dt><dd>{rate(s.dl_bps)}</dd></div></dl>
      {trend !== undefined && <section className="xcn-detail-trend" aria-label="会话速率走势">
        <div className="xcn-panel-heading"><h3>{qfi === null ? '会话速率走势' : '承载速率走势 · QFI ' + qfi}</h3>
          <select aria-label="详情走势对象" value={qfi === null ? 'session' : String(qfi)}
            onChange={e => this.change({detailQfi: e.target.value === 'session' ? null : Number(e.target.value), detail: null})}>
            <option value="session">整个会话</option>
            {(s.bearers || []).map(b => <option key={b.qfi} value={String(b.qfi)}>QFI {b.qfi}</option>)}
            {qfi !== null && !(s.bearers || []).some(b => b.qfi === qfi) && <option value={String(qfi)}>QFI {qfi} · 已释放</option>}
          </select>{this.windowPicker()}</div>
        <Chart theme={this.props.theme} trend={trend}/></section>}
      <SessionPolicyStatus session={s}/>
      <section className="xcn-session-forwarding" aria-label="承载与规则">
      <h3>承载与规则</h3><h4>QoS 流</h4>{(s.bearers || []).length ? <div className="xcn-table-scroll"><table className="xcn-table" aria-label="会话 QoS 流">
        <thead><tr><th>QFI</th><th>类型</th><th>5QI</th><th>上行</th><th>下行</th><th>走势</th></tr></thead>
        <tbody>{s.bearers.map(b => <tr key={b.qfi}><td>{value(b.qfi)}</td><td>{(() => {
          const q = (s.qos_flows || []).find(q => q.qfi === b.qfi);
          return q && typeof q.is_default === 'boolean' ? q.is_default ? '默认流' : '专载' : '—';
        })()}</td><td>{value(b['5qi'])}</td>
          <td>{rate(b.ul_bps)}</td><td>{rate(b.dl_bps)}</td><td>
            <button type="button" className="xcn-link" aria-label={'查看 QFI ' + b.qfi + ' 的流量走势'}
              onClick={() => trend === undefined ? this.select({...s, qfi: b.qfi}, 'sessions') :
                this.change({detailQfi: b.qfi, detail: null})}>查看走势</button></td></tr>)}</tbody></table></div> : <Empty>暂无承载数据。</Empty>}
      <Rules rules={s.rules} formatRate={rate}/>
      </section>
      {this.canControl() && <SessionControls key={s.supi + "/" + s.psi + "/" + s.seid} session={s}/>}
    </section>;
  }
  detail() {
    const {selected, detail} = this.state;
    if (!selected) return null;
    const type = selected.type;
    const sessions = detail ? detail[0].items : [], events = detail ? detail[1].items : [];
    const userScope = type === 'sessions' && !selected.row.seid;
    let r = selected.row;
    const rows = (this.state.data || {}).items || [];
    const session = sessions.find(s => String(s.seid) === String(r.seid) && s.supi === r.supi);
    if (type === 'ues' && detail) r = detail[3].items[0] || r;
    else if (type === 'sessions' && !userScope) r = session || r;
    else if (type === 'sessions' && this.props.view === type) r = rows.find(s => s.supi === r.supi && !s.seid) || r;
    else if (type === 'gnbs' && this.props.view === type) r = rows.find(s => s.gnb_id === r.gnb_id && s.plmn === r.plmn && s.gnb_id_bits === r.gnb_id_bits) || r;
    // Event identity is event_id, never SUPI: one terminal has many events.
    const id = type === 'events' ? r.name : r.supi || (r.gnb_id !== undefined ? r.gnb_id : r.name);
    const raw = userScope ? {...r, sessions} : r;
    return <Drawer title={type === 'sessions' ? userScope ? '用户详情' : '会话详情' :
      (titles[type] || '详情') + ' · ' + value(id)} onClose={this.close}>
      {r.supi && type !== 'events' && <div><div className="xcn-detail-identity"><span className="xcn-mono">{r.supi}</span><Status state={r.cm_state}/></div>
        {(type !== 'sessions' || userScope) && <div className="xcn-detail-trend">
          <div className="xcn-panel-heading"><h3>用户速率走势</h3>{this.windowPicker()}</div>
          <Chart theme={this.props.theme} trend={detail && detail[2]}/></div>}</div>}
      {type === 'ues' && r.supi && this.canControl() && <UserRelease key={r.supi} supi={r.supi}/>}
      {type === 'ues' && <div><h3>接入与位置</h3><dl className="xcn-fields"><dt>位置状态</dt><dd>{value((r.location || {}).status)}</dd>
        <dt>gNB / PLMN</dt><dd className="xcn-mono">{value((r.gnb || (r.location || {}).gnb || {}).gnb_id)} / {value((r.gnb || (r.location || {}).gnb || {}).plmn)}</dd>
        <dt>小区 NCI</dt><dd className="xcn-mono">{value(((r.location || {}).nr_cgi || {}).nci_hex)}</dd></dl>
        <h3>AMF 当前 PDU 会话</h3>{(r.pdu_sessions || []).map((p, i) => <div className="xcn-detail-session" key={i}>
          <b>PSI {p.psi} / {p.dnn}</b><p className="xcn-mono">IPv4 {value(p.ipv4)}　IPv6 {value(p.ipv6)}</p><Raw data={p}/></div>)}</div>}
      {type === 'gnbs' && <div><h3>关联终端</h3>{(r.associated_ues || []).map((u, i) => <div className="xcn-detail-session" key={i}>
        <button type="button" className="xcn-link xcn-mono" onClick={() => this.select(u, 'ues')}>{u.supi}</button> <Status state={u.cm_state}/></div>)}
        <h3>观测到的小区</h3><Raw data={r.observed_cells}/>
        <Button onClick={() => this.navigate('ues', {identityFilters: {gnb_id: String(r.gnb_id), plmn: r.plmn, gnb_id_bits: String(r.gnb_id_bits)}})}>查看此基站的终端</Button></div>}
      {type === 'sessions' && <div>
        {!detail ? <Empty>正在读取会话详情…</Empty> : !userScope && !session ?
          <div><Empty>该会话已释放或不在当前采集结果中，请在列表中选择当前会话。</Empty>
            {this.state.detailQfi !== null && <Chart theme={this.props.theme} trend={detail[2]}/>}</div> :
          (userScope ? sessions : [session]).map(s => this.sessionRules(s, userScope ? undefined : detail[2]))}
        {detail && userScope && !sessions.length && <Empty>暂无匹配的会话。</Empty>}</div>}
      {type === 'events' && <div><dl className="xcn-fields xcn-event-fields">
        <dt>时间</dt><dd>{dateTime(Number(r.timestamp_us) / 1e6)}.{eventClock(r.timestamp_us).split('.')[1]}</dd>
        <dt>协议 / 网元</dt><dd>{value(r.protocol)} / {String(r.nf || '').toUpperCase()}</dd>
        <dt>方向</dt><dd>{value(r.direction)}</dd><dt>结果</dt><dd><Status state={r.result}/></dd>
        <dt>终端</dt><dd><button className="xcn-link xcn-mono" type="button" disabled={!r.supi} onClick={() => this.select({supi: r.supi}, 'ues')}>{value(r.supi)}</button></dd>
        <dt>详情</dt><dd>{value(r.info)}</dd>{r.cause && <dt>原因</dt>}{r.cause && <dd>{r.cause}</dd>}
      </dl><SignalDetails key={r.event_id} event={r}/></div>}
      {r.supi && type === 'ues' && <div><h3>UPF 会话</h3>{sessions.length ? sessions.map(s => <div className="xcn-detail-session" key={s.seid}>
        <button type="button" className="xcn-link" onClick={() => this.select(s, 'sessions')}>PSI {value(s.psi)} / {s.dnn}</button>
        <p className="xcn-mono">SEID {s.seid}　{value(s.ue_ip)}</p><p>{rate(s.ul_bps)} ↑　{rate(s.dl_bps)} ↓</p></div>) : <Empty>{detail ? '暂无匹配的 UPF 会话。' : '正在读取会话…'}</Empty>}</div>}
      <Raw data={raw} label={type === 'events' ? '查看信令记录（JSON）' : undefined}/>
      {r.supi && type !== 'events' && <div><h3>终端近期信令</h3>
        <Events compact items={events} onSelect={e => this.select(e, 'events')}/></div>}
    </Drawer>;
  }
  render() {
    const {view} = this.props;
    const {data, error, loading, updated, page} = this.state;
    return <main className="xcn-monitor">
      <div className="xcn-page-heading"><h1>{titles[view]}</h1>
        <div className="xcn-refresh"><span className={'xcn-live-dot ' + (error ? 'warn' : '')}/>
          {typeof document !== 'undefined' && document.hidden ? '后台已暂停刷新' : '约 2 秒更新'}<small>页面更新 {updated ? clock(updated / 1000) : '等待数据'}</small></div></div>
      {error && <div className="xcn-alert warn" role="status">{error}<Button onClick={() => this.change({})}>重试</Button></div>}
      {['ues', 'gnbs', 'sessions', 'events'].indexOf(view) >= 0 && this.filters()}
      {view === 'ues' && this.canControl() && <BulkRelease/>}
      {view === 'sessions' && <div className="xcn-tabs" role="group" aria-label="速率层级">
        {[['user', '用户'], ['session', '会话'], ['bearer', '承载'], ['rule', '规则']].map(([k, v]) => <button key={k} type="button"
          aria-pressed={this.state.level === k} onClick={() => this.change({level: k, page: 0})}>{v}</button>)}</div>}
      {view === 'sessions' && this.state.level === 'rule' && <label className="xcn-rule-scope">规则范围
        <select aria-label="规则范围" value={this.state.rule_scope} onChange={e => this.change({rule_scope: e.target.value, page: 0})}>
          <option value="business">业务规则</option><option value="internal">内部辅助规则</option><option value="all">全部规则</option>
        </select></label>}
      {!data && <Empty>{loading ? '正在读取监控数据…' : '监控数据尚未就绪，请检查采集服务是否启用。'}</Empty>}
      {data && view === 'overview' && this.overview(data)}
      {data && view === 'system' && this.system(data)}
      {data && ['ues', 'gnbs', 'sessions', 'events'].indexOf(view) >= 0 && <section className="xcn-panel">
        {view === 'sessions' && ['user', 'session', 'bearer'].includes(this.state.level) && <div className="xcn-panel-heading xcn-session-list-heading">
          <h2>{this.state.level === 'user' ? '用户' : this.state.level === 'bearer' ? '承载' : '会话'}列表</h2>
          <span>点击{this.state.level === 'user' ? '用户' : this.state.level === 'bearer' ? '承载' : '会话'}行切换走势，点击 SUPI 查看详情</span></div>}
        {view === 'events' && data.coverage && <div className="xcn-event-range"><span>信令覆盖</span>
          <span>{data.coverage.oldest_at ? dateTime(data.coverage.oldest_at) + ' — ' + dateTime(data.coverage.newest_at) : '暂无信令'}</span></div>}
        {view === 'events' ? <Events items={data.items} onSelect={e => this.select(e, 'events')}/> : this.table(view, data)}
        <div className="xcn-pagination"><span>共 {data.total} 条 · 第 {page + 1} 页</span><div>
          <Button disabled={page === 0} onClick={() => this.change({page: page - 1})}>上一页</Button>
          <Button disabled={(page + 1) * 50 >= data.total} onClick={() => this.change({page: page + 1})}>下一页</Button></div></div>
      </section>}
      {data && view === 'sessions' && ['user', 'session', 'bearer'].includes(this.state.level) && this.trafficTrend(data)}
      {this.detail()}
    </main>;
  }
}
