import {Component} from 'react';
import axios from 'axios';
import Chart from './Chart';
import {get} from './client';

const titles = {overview: '概览', ues: '终端', gnbs: '基站', sessions: '会话', events: '事件', system: '状态'};
const sourceNames = {amf_ues: '终端', smf_pdus: '会话', amf_gnbs: '基站', upf_rates: '用户面速率',
  amf_events: 'AMF 事件', smf_events: 'SMF 事件', pods: '容器'};
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
class Raw extends Component {
  state = {expanded: false};
  render() {
    return <details className="xcn-raw" open={this.state.expanded}>
      <summary onClick={e => { e.preventDefault(); this.setState({expanded: !this.state.expanded}); }}>查看完整字段</summary>
      {this.state.expanded && <pre>{JSON.stringify(this.props.data, null, 2)}</pre>}
    </details>;
  }
}
const Button = ({children, onClick, ...rest}) => <button type="button" className="xcn-button" onClick={onClick} {...rest}>{children}</button>;

function Events({items, onSelect, compact}) {
  if (!items || !items.length) return <Empty>暂无匹配事件，可切换协议或调整搜索条件。</Empty>;
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
      const list = Array.from(this.panel.querySelectorAll('button, a, input, select, summary, [tabindex="0"]'))
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
  state = {data: null, trend: null, error: null, loading: true, page: 0, q: '', search: '',
    cm_state: '', protocol: 'NGAP', level: 'session', seconds: 1800, selected: null, detail: null, identityFilters: {}};
  componentDidMount() {
    this.active = true;
    document.addEventListener('visibilitychange', this.visibility);
    this.poll();
  }
  componentWillReceiveProps(next) {
    if (next.view !== this.props.view) {
      this.stop();
      const filters = this.navigateFilters || {};
      this.navigateFilters = null;
      this.setState({data: null, trend: null, error: null, page: 0, q: '', search: '', cm_state: '', protocol: 'NGAP',
        selected: null, detail: null, identityFilters: {}, ...filters}, () => this.schedule(0));
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
  poll = async () => {
    if (!this.active || document.hidden) return;
    const generation = this.generation;
    const {view} = this.props;
    const {q, page, cm_state, protocol, level, seconds, selected} = this.state;
    this.cancel = axios.CancelToken.source();
    const token = this.cancel.token;
    try {
      const params = {q, page, limit: 50, cm_state, ...this.state.identityFilters};
      if (view === 'events' || view === 'overview') params.protocol = protocol;
      const endpoint = view === 'sessions' ? 'rates' : view;
      if (view === 'sessions') params.level = level;
      const calls = [get(endpoint, params, token)];
      const trending = view === 'overview' || view === 'sessions';
      if (trending) calls.push(get('trends', {seconds}, token));
      const needsDetail = selected && selected.row.supi && selected.type !== 'events';
      if (needsDetail) {
        calls.push(Promise.all([get('sessions', {supi: selected.row.supi, limit: 100}, token),
          get('events', {supi: selected.row.supi, limit: 20}, token),
          get('trends', {key: selected.type === 'sessions' && selected.row.seid ?
            'session:' + selected.row.seid : 'user:' + selected.row.supi, seconds}, token),
          get('ues', {supi: selected.row.supi, limit: 1}, token)]));
      }
      const results = await Promise.all(calls);
      if (!this.active || generation !== this.generation) return;
      this.setState({data: results[0], trend: trending ? results[1] : null,
        detail: needsDetail ? results[trending ? 2 : 1] : null,
        error: null, loading: false, updated: Date.now()});
    } catch (e) {
      if (!this.active || generation !== this.generation || axios.isCancel(e)) return;
      this.setState({error: e.response && e.response.data && e.response.data.error || '监控连接中断，等待恢复。', loading: false});
    } finally {
      if (this.active && generation === this.generation) this.schedule(2000 + Math.random() * 200);
    }
  };
  change = change => { this.stop(); this.setState({...change, loading: true}, () => this.schedule(0)); };
  search = e => { e.preventDefault(); this.change({q: this.state.search.trim(), page: 0}); };
  select = (row, type) => this.change({selected: {row, type}, detail: null});
  close = () => this.change({selected: null, detail: null});
  navigate = (view, filters = {}) => {
    if (view === this.props.view) this.change({...filters, selected: null, page: 0});
    else { this.navigateFilters = filters; this.props.onNavigate(view); }
  };
  windowPicker() {
    return <select aria-label="趋势时间范围" value={this.state.seconds}
      onChange={e => this.change({seconds: Number(e.target.value)})}>
      <option value={300}>最近 5 分钟</option><option value={900}>最近 15 分钟</option><option value={1800}>最近 30 分钟</option>
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
        <Chart theme={this.props.theme} points={(this.state.trend || {}).points}/>
      </section>
      <section className="xcn-panel"><div className="xcn-panel-heading"><h2>近期控制面活动</h2>
        <div className="xcn-panel-actions">{this.protocolPicker(data)}<Button onClick={() => this.navigate('events', {protocol: this.state.protocol})}>更多事件</Button></div></div>
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
    const base = [['终端 SUPI', r => <button className="xcn-link xcn-mono" type="button" onClick={() => this.select(r, 'sessions')}>{value(r.supi)}</button>],
      ['连接状态', r => <Status state={r.cm_state}/>]];
    if (this.state.level !== 'user') base.push(['PSI / SEID', r => <span className="xcn-mono">{value(r.psi)} / {value(r.seid)}</span>]);
    else base.push(['会话数', r => value(r.session_count)]);
    if (this.state.level === 'bearer' || this.state.level === 'rule') base.push(['QFI / 5QI', r => value(r.qfi) + ' / ' + value(r['5qi'])]);
    if (this.state.level === 'rule') return base.concat([['PDR / QER', r => value(r.pdr_id) + ' / ' + value(r.qer_id)],
      ['方向', r => r.direction], ['速率', r => rate(r.bps)], ['累计字节', r => <span className="xcn-mono">{value(r.bytes)}</span>]]);
    return base.concat([['DNN', r => value(r.dnn)], ['上行', r => rate(r.ul_bps)], ['下行', r => rate(r.dl_bps)],
      ['累计字节 ↑ / ↓', r => <span className="xcn-mono">{value(r.ul_bytes)} / {value(r.dl_bytes)}</span>]]);
  }
  table(view, data) {
    const columns = this.columns(view);
    if (!data.items || !data.items.length) return <Empty/>;
    return <div className="xcn-table-scroll"><table className="xcn-table"><thead><tr>{columns.map(([title]) => <th key={title} scope="col">{title}</th>)}</tr></thead>
      <tbody>{data.items.map((row, i) => <tr key={row.event_id || [row.supi, row.seid, row.qfi, row.pdr_id, row.gnb_id, i].join('/')}>
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
        <div className="xcn-memory"><div><span>缓存内存</span><strong>{mib(memory.used_bytes)}<small> / {mib(memory.limit_bytes)}</small></strong></div>
          <progress value={used} max={100} aria-label="缓存内存使用率"/>
          <span>启动于 {dateTime(h.started_at)}</span></div>
        {memory.last_reason && <p className="xcn-panel-note">最近保护原因：{memory.last_reason}</p>}</section>
      <section className="xcn-panel"><div className="xcn-panel-heading"><h2>数据源</h2></div>
        <div className="xcn-table-scroll"><table className="xcn-table"><thead><tr><th>数据</th><th>状态</th><th>最近更新</th><th>更新周期</th><th>说明</th></tr></thead>
          <tbody>{Object.keys(sources).filter(visibleSource).map(k => <tr key={k}><td>{sourceNames[k] || k}</td>
            <td><Status state={sources[k].error ? '异常' : sources[k].stale ? '等待更新' : 'ready'}/></td>
            <td className="xcn-nowrap">{clock(sources[k].updated_at)}</td><td>{sources[k].interval} 秒</td>
            <td className="xcn-wrap">{sources[k].error || (sources[k].gap_count ? '检测到事件缺口 ' + sources[k].gap_count + ' 次' :
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
  sessionRules(s) {
    return <section className="xcn-session-detail" key={s.seid}>
      <div className="xcn-session-heading"><h4>PSI {value(s.psi)}</h4><span className="xcn-dnn">{value(s.dnn)}</span>
        <span className="xcn-mono">SEID {value(s.seid)}</span></div>
      <dl className="xcn-fields"><dt>终端地址</dt><dd className="xcn-mono">{value(s.ue_ip)}</dd>
        <dt>上行 / 下行</dt><dd>{rate(s.ul_bps)} / {rate(s.dl_bps)}</dd></dl>
      <h4>承载</h4>{(s.bearers || []).length ? <div className="xcn-table-scroll"><table className="xcn-table">
        <thead><tr><th>QFI</th><th>5QI</th><th>上行</th><th>下行</th></tr></thead>
        <tbody>{s.bearers.map(b => <tr key={b.qfi}><td>{value(b.qfi)}</td><td>{value(b['5qi'])}</td>
          <td>{rate(b.ul_bps)}</td><td>{rate(b.dl_bps)}</td></tr>)}</tbody></table></div> : <Empty>暂无承载数据。</Empty>}
      <h4>规则</h4>{(s.rules || []).length ? <div className="xcn-table-scroll"><table className="xcn-table">
        <thead><tr><th>QFI / 5QI</th><th>PDR / QER</th><th>方向</th><th>速率</th><th>累计字节</th></tr></thead>
        <tbody>{s.rules.map((rule, i) => <tr key={i}><td>{value(rule.qfi)} / {value(rule['5qi'])}</td>
          <td>{value(rule.pdr_id)} / {value(rule.qer_id)}</td><td>{rule.direction}</td><td>{rate(rule.bps)}</td>
          <td className="xcn-mono">{rule.bytes}</td></tr>)}</tbody></table></div> : <Empty>暂无规则数据。</Empty>}
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
    if (type === 'ues' && detail) r = detail[3].items[0] || r;
    else if (type === 'sessions' && !userScope) r = sessions.find(s => s.seid === r.seid) || r;
    else if (type === 'sessions' && this.props.view === type) r = rows.find(s => s.supi === r.supi && !s.seid) || r;
    else if (type === 'gnbs' && this.props.view === type) r = rows.find(s => s.gnb_id === r.gnb_id && s.plmn === r.plmn && s.gnb_id_bits === r.gnb_id_bits) || r;
    // Event identity is event_id, never SUPI: one terminal has many events.
    const id = type === 'events' ? r.name : r.supi || (r.gnb_id !== undefined ? r.gnb_id : r.name);
    const raw = userScope ? {...r, sessions} : r;
    return <Drawer title={(userScope ? '用户' : titles[type] || '详情') + ' · ' + value(id)} onClose={this.close}>
      {r.supi && type !== 'events' && <div><div className="xcn-detail-identity"><span className="xcn-mono">{r.supi}</span><Status state={r.cm_state}/></div>
        <div className="xcn-panel-heading"><h3>速率趋势</h3>{this.windowPicker()}</div>
        <Chart theme={this.props.theme} points={detail && detail[2].points}/></div>}
      {type === 'ues' && <div><h3>接入与位置</h3><dl className="xcn-fields"><dt>位置状态</dt><dd>{value((r.location || {}).status)}</dd>
        <dt>gNB / PLMN</dt><dd className="xcn-mono">{value((r.gnb || (r.location || {}).gnb || {}).gnb_id)} / {value((r.gnb || (r.location || {}).gnb || {}).plmn)}</dd>
        <dt>小区 NCI</dt><dd className="xcn-mono">{value(((r.location || {}).nr_cgi || {}).nci_hex)}</dd></dl>
        <h3>AMF 当前 PDU 会话</h3>{(r.pdu_sessions || []).map((p, i) => <div className="xcn-detail-session" key={i}>
          <b>PSI {p.psi} / {p.dnn}</b><p className="xcn-mono">IPv4 {value(p.ipv4)}　IPv6 {value(p.ipv6)}</p><Raw data={p}/></div>)}</div>}
      {type === 'gnbs' && <div><h3>关联终端</h3>{(r.associated_ues || []).map((u, i) => <div className="xcn-detail-session" key={i}>
        <button type="button" className="xcn-link xcn-mono" onClick={() => this.select(u, 'ues')}>{u.supi}</button> <Status state={u.cm_state}/></div>)}
        <h3>观测到的小区</h3><Raw data={r.observed_cells}/>
        <Button onClick={() => this.navigate('ues', {identityFilters: {gnb_id: String(r.gnb_id), plmn: r.plmn, gnb_id_bits: String(r.gnb_id_bits)}})}>查看此基站的终端</Button></div>}
      {type === 'sessions' && <div><h3>承载与规则</h3>
        {!detail ? <Empty>正在读取会话详情…</Empty> :
          (userScope ? sessions : [r]).map(s => this.sessionRules(s))}
        {detail && userScope && !sessions.length && <Empty>暂无匹配的会话。</Empty>}</div>}
      {type === 'events' && <div><dl className="xcn-fields xcn-event-fields">
        <dt>时间</dt><dd>{dateTime(Number(r.timestamp_us) / 1e6)}.{eventClock(r.timestamp_us).split('.')[1]}</dd>
        <dt>协议 / 网元</dt><dd>{value(r.protocol)} / {String(r.nf || '').toUpperCase()}</dd>
        <dt>方向</dt><dd>{value(r.direction)}</dd><dt>结果</dt><dd><Status state={r.result}/></dd>
        <dt>终端</dt><dd><button className="xcn-link xcn-mono" type="button" disabled={!r.supi} onClick={() => this.select({supi: r.supi}, 'ues')}>{value(r.supi)}</button></dd>
        <dt>详情</dt><dd>{value(r.info)}</dd>{r.cause && <dt>原因</dt>}{r.cause && <dd>{r.cause}</dd>}
      </dl></div>}
      {r.supi && type === 'ues' && <div><h3>UPF 会话</h3>{sessions.length ? sessions.map(s => <div className="xcn-detail-session" key={s.seid}>
        <button type="button" className="xcn-link" onClick={() => this.select(s, 'sessions')}>PSI {value(s.psi)} / {s.dnn}</button>
        <p className="xcn-mono">SEID {s.seid}　{value(s.ue_ip)}</p><p>{rate(s.ul_bps)} ↑　{rate(s.dl_bps)} ↓</p></div>) : <Empty>{detail ? '暂无匹配的 UPF 会话。' : '正在读取会话…'}</Empty>}</div>}
      <Raw data={raw}/>
      {r.supi && type !== 'events' && <div><h3>终端近期事件</h3>
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
      {view === 'sessions' && <div className="xcn-tabs" role="group" aria-label="速率层级">
        {[['user', '用户'], ['session', '会话'], ['bearer', '承载'], ['rule', '规则']].map(([k, v]) => <button key={k} type="button"
          aria-pressed={this.state.level === k} onClick={() => this.change({level: k, page: 0})}>{v}</button>)}</div>}
      {!data && <Empty>{loading ? '正在读取监控数据…' : '监控数据尚未就绪，请检查采集服务是否启用。'}</Empty>}
      {data && view === 'overview' && this.overview(data)}
      {data && view === 'system' && this.system(data)}
      {data && ['ues', 'gnbs', 'sessions', 'events'].indexOf(view) >= 0 && <section className="xcn-panel">
        {view === 'events' && data.coverage && <div className="xcn-event-range"><span>事件覆盖</span>
          <span>{data.coverage.oldest_at ? dateTime(data.coverage.oldest_at) + ' — ' + dateTime(data.coverage.newest_at) : '暂无事件'}</span></div>}
        {view === 'events' ? <Events items={data.items} onSelect={e => this.select(e, 'events')}/> : this.table(view, data)}
        <div className="xcn-pagination"><span>共 {data.total} 条 · 第 {page + 1} 页</span><div>
          <Button disabled={page === 0} onClick={() => this.change({page: page - 1})}>上一页</Button>
          <Button disabled={(page + 1) * 50 >= data.total} onClick={() => this.change({page: page + 1})}>下一页</Button></div></div>
      </section>}
      {data && view === 'sessions' && <section className="xcn-panel"><div className="xcn-panel-heading"><h2>全局流量趋势</h2>{this.windowPicker()}</div>
        <Chart theme={this.props.theme} points={(this.state.trend || {}).points}/></section>}
      {this.detail()}
    </main>;
  }
}
