import {Component} from 'react';
import axios from 'axios';
import Chart from './Chart';
import {get} from './client';

const titles = {overview: '运行概览', ues: 'UE 终端', gnbs: 'gNB 接入',
  sessions: '会话与速率', events: '事件时间线', system: '系统状态'};
const hints = {overview: '从接入到用户面，查看核心网此刻的运行状态。',
  ues: '连接状态来自 AMF，上行与下行速率来自 UPF。',
  gnbs: '查看 NG 接口、支持的 TA 与切片，以及关联终端。',
  sessions: '按用户、会话、承载或规则查看速率与累计流量。',
  events: 'AMF 与 SMF 的控制面事件，按源内顺序保留。',
  system: '查看网元、容器、采集状态与数据覆盖范围。'};
const value = v => v === null || v === undefined || v === '' ? '—' : String(v);
const clock = v => v ? new Date(Number(v) * 1000).toLocaleTimeString('zh-CN', {hour12: false}) : '尚未采集';
const dateTime = v => v ? new Date(Number(v) * 1000).toLocaleString('zh-CN', {hour12: false}) : '尚未采集';
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
const Raw = ({data}) => <details className="xcn-raw"><summary>查看完整字段</summary><pre>{JSON.stringify(data, null, 2)}</pre></details>;
const Button = ({children, onClick, ...rest}) => <button type="button" className="xcn-button" onClick={onClick} {...rest}>{children}</button>;

function Events({items, onSelect}) {
  if (!items || !items.length) return <Empty>暂时没有事件。采集启动后会显示控制面活动。</Empty>;
  return <ol className="xcn-events">{items.map((e, i) => <li key={e.event_id || i} className={bad(e.result) ? 'problem' : ''}>
    <time>{clock(Number(e.timestamp_us) / 1e6)}</time>
    <div><button type="button" className="xcn-link" onClick={() => onSelect(e)}>{value(e.name)}</button>
      <p><span className="xcn-mono">{e.supi || '身份解析中'}</span><span>{value(e.nf)} / {value(e.protocol)}</span></p>
      {e.info && <p className="xcn-event-info">{e.info}</p>}</div>
    <Status state={e.result}/>
  </li>)}</ol>;
}

class Drawer extends Component {
  componentDidMount() {
    this.before = document.activeElement;
    this.close.focus();
    document.addEventListener('keydown', this.key);
  }
  componentWillUnmount() {
    document.removeEventListener('keydown', this.key);
    if (this.before && document.contains(this.before)) this.before.focus();
  }
  key = e => {
    if (e.key === 'Escape') this.props.onClose();
    if (e.key === 'Tab') {
      const list = this.panel.querySelectorAll('button, a, input, select, summary, [tabindex="0"]');
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
    cm_state: '', nf: '', level: 'session', seconds: 1800, selected: null, detail: null, identityFilters: {}};
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
      this.setState({data: null, trend: null, error: null, page: 0, q: '', search: '', cm_state: '', nf: '',
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
    const {q, page, cm_state, nf, level, seconds, selected} = this.state;
    this.cancel = axios.CancelToken.source();
    const token = this.cancel.token;
    try {
      const params = {q, page, limit: 50, cm_state, nf, ...this.state.identityFilters};
      const endpoint = view === 'sessions' ? 'rates' : view;
      if (view === 'sessions') params.level = level;
      const calls = [get(endpoint, params, token)];
      const trending = view === 'overview' || view === 'sessions';
      if (trending) calls.push(get('trends', {seconds}, token));
      if (selected && selected.row.supi) {
        calls.push(Promise.all([get('sessions', {supi: selected.row.supi}, token),
          get('events', {supi: selected.row.supi, limit: 20}, token),
          get('trends', {key: selected.type === 'sessions' && selected.row.seid ?
            'session:' + selected.row.seid : 'user:' + selected.row.supi, seconds}, token)]));
      }
      const results = await Promise.all(calls);
      if (!this.active || generation !== this.generation) return;
      this.setState({data: results[0], trend: trending ? results[1] : null,
        detail: selected && selected.row.supi ? results[trending ? 2 : 1] : null,
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
  overview(data) {
    const states = data.cm_states || {};
    const health = data.health || {};
    const problems = Object.keys(health.sources || {}).filter(k => health.sources[k].stale || health.sources[k].error);
    return <div>
      <section className="xcn-network" aria-label="核心网关联概览">
        <div className="xcn-network-access"><span>接入网络</span><button type="button" onClick={() => this.navigate('gnbs')}>
          <strong>{data.gnb_count}</strong><span>gNB</span></button>
          <button type="button" onClick={() => this.navigate('ues')}><strong>{data.ue_count}</strong><span>UE 终端</span></button></div>
        <div className="xcn-network-path"><span>NGAP</span><div/><span>GTP-U</span></div>
        <div className="xcn-network-core"><div className="xcn-control-nodes"><b>AMF</b><span>移动性管理</span><b>SMF</b><span>会话管理</span></div>
          <div className="xcn-user-node"><b>UPF</b><span>用户面转发</span><strong>{data.session_count}<small> PDU 会话</small></strong></div></div>
        <div className="xcn-network-state"><span>终端连接状态</span><p><i className="good"/>已连接 <b>{states.connected || 0}</b></p>
          <p><i className="neutral"/>空闲 <b>{states.idle || 0}</b></p><p><i className="warn"/>未知 <b>{states.unknown || 0}</b></p></div>
      </section>
      {problems.length > 0 && <div className="xcn-alert warn" role="status">{problems.length} 个数据源需要关注：{problems.join('、')}。
        <button type="button" className="xcn-link" onClick={() => this.navigate('system')}>查看采集状态</button></div>}
      <section className="xcn-panel"><div className="xcn-panel-heading"><div><h2>用户面流量</h2>
        <p>上行 <b className="xcn-ul">{rate(data.ul_bps)}</b><span className="xcn-divider"/>下行 <b className="xcn-dl">{rate(data.dl_bps)}</b></p></div>{this.windowPicker()}</div>
        <Chart theme={this.props.theme} points={(this.state.trend || {}).points}/>
      </section>
      <section className="xcn-panel"><div className="xcn-panel-heading"><div><h2>近期控制面活动</h2><p>不同网元的时间可能存在偏差，以源内序号追踪事件。</p></div>
        <Button onClick={() => this.navigate('events')}>全部事件</Button></div>
        <Events items={data.events} onSelect={e => this.select(e, 'events')}/></section>
      <p className="xcn-footnote">事件按容量保留，流量趋势约 30 分钟；达到内存水位时优先清理历史。采集启动于 {dateTime(health.started_at)}；缓存不持久化。</p>
    </div>;
  }
  columns(view) {
    if (view === 'ues') return [['终端 SUPI', r => <button className="xcn-link xcn-mono" type="button" onClick={() => this.select(r, view)}>{value(r.supi)}</button>],
      ['连接状态', r => <Status state={r.cm_state}/>], ['接入 gNB', r => {
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
      {view === 'events' && <select aria-label="网元筛选" value={this.state.nf} onChange={e => this.change({nf: e.target.value, page: 0})}>
        <option value="">全部网元</option><option value="amf">AMF</option><option value="smf">SMF</option></select>}
      {(this.state.q || Object.keys(this.state.identityFilters).length > 0) && <Button onClick={() => this.change({q: '', search: '', page: 0, identityFilters: {}})}>清除筛选</Button>}
    </div>;
  }
  system(data) {
    const h = data.health || {}, sources = h.sources || {}, usage = data.usage || [], memory = h.memory || {};
    const mib = bytes => bytes === undefined ? '—' : (bytes / 1048576).toFixed(1) + ' MiB';
    return <div>
      {h.affinity && !h.affinity.verified && <div className="xcn-alert warn">{h.affinity.error || 'CPU 隔离尚未验证'}</div>}
      <section className="xcn-panel"><div className="xcn-panel-heading"><div><h2>监控缓存内存</h2>
        <p>{memory.paused ? '内存保护中 · 暂停新采集' : '内存水位监测中'} · 已用 {mib(memory.used_bytes)} / 限制 {mib(memory.limit_bytes)}</p></div></div>
        <p className="xcn-panel-note">保护水位 {mib(memory.high_bytes)}；降至 {mib(memory.resume_bytes)} 后恢复。
          已清理历史 {memory.reclaims || 0} 次，拒绝新批次 {memory.skipped_batches || 0} 次。最新有效快照继续保留。</p>
        {memory.last_reason && <p className="xcn-panel-note">最近保护原因：{memory.last_reason}</p>}</section>
      <section className="xcn-panel"><div className="xcn-panel-heading"><div><h2>采集与覆盖范围</h2><p>{h.enabled ? '采集已启用' : '采集已暂停'} · 启动于 {clock(h.started_at)}</p></div></div>
        <div className="xcn-table-scroll"><table className="xcn-table"><thead><tr><th>数据源</th><th>最近成功</th><th>状态</th><th>采集次数</th><th>说明</th></tr></thead>
          <tbody>{Object.keys(sources).map(k => <tr key={k}><td className="xcn-mono">{k}</td><td>{clock(sources[k].updated_at)}</td>
            <td><Status state={sources[k].stale ? '陈旧' : sources[k].error ? '异常' : 'ready'}/></td><td>{sources[k].requests}</td>
            <td className="xcn-wrap">{sources[k].error || (sources[k].gap_count ? '检测到事件缺口 ' + sources[k].gap_count + ' 次' :
              sources[k].reset_count ? '网元序列重置 ' + sources[k].reset_count + ' 次' : '周期 ' + sources[k].interval + ' 秒')}
              {k === 'upf_rates' && sources[k].lock_hold_us !== undefined && sources[k].lock_hold_us !== null &&
                <small className="xcn-subtext">采样锁等待 {sources[k].lock_wait_us} μs / 持有 {sources[k].lock_hold_us} μs</small>}</td></tr>)}</tbody></table></div>
        <p className="xcn-panel-note">{h.coverage}<br/>保留 {h.events || 0} 条事件，已淘汰 {h.event_evictions || 0} 条；趋势 {h.curves || 0} 条，已淘汰 {h.curve_evictions || 0} 条。
          {h.event_coverage && <span><br/>事件覆盖：{dateTime(h.event_coverage.oldest_at)} 至 {dateTime(h.event_coverage.newest_at)}。</span>}
          {h.affinity && h.affinity.cpus && <span> 监控 CPU：<span className="xcn-mono">{h.affinity.cpus.join(',')}</span></span>}</p></section>
      <section className="xcn-panel"><div className="xcn-panel-heading"><h2>容器状态</h2></div>
        {!data.pods || !data.pods.length ? <Empty>尚未获取容器状态。</Empty> : <div className="xcn-table-scroll"><table className="xcn-table">
          <thead><tr><th>Pod / 容器</th><th>节点</th><th>就绪</th><th>重启</th><th>CPU / 内存</th></tr></thead>
          <tbody>{data.pods.map(p => (p.containers || []).map(c => {
            const u = (usage.find(x => x.name === p.name) || {}).containers || [];
            const cu = (u.find(x => x.name === c.name) || {}).usage || {};
            return <tr key={p.name + c.name}><td><span className="xcn-mono">{c.name}</span><small className="xcn-subtext">{p.name}</small></td>
              <td>{p.node}</td><td><Status state={c.ready ? 'ready' : p.phase}/></td><td>{c.restarts}</td>
              <td className="xcn-mono">{cu.cpu ? cu.cpu + ' / ' + cu.memory : 'Metrics API 不可用'}</td></tr>;
          }))}</tbody></table></div>}</section>
      <section className="xcn-panel"><div className="xcn-panel-heading"><h2>网元指标</h2></div>
        {Object.keys(data.metrics || {}).map(k => <details className="xcn-metrics" key={k}><summary>{k.replace('_metrics', '').toUpperCase()} 指标（{data.metrics[k].length}）</summary>
          <div className="xcn-table-scroll"><table className="xcn-table"><thead><tr><th>指标</th><th>值</th></tr></thead><tbody>
            {data.metrics[k].filter(m => !this.state.q || m.name.toLowerCase().indexOf(this.state.q.toLowerCase()) >= 0).slice(0, 100)
              .map(m => <tr key={m.name}><td className="xcn-mono xcn-wrap">{m.name}</td><td className="xcn-mono">{m.value}</td></tr>)}
          </tbody></table></div><p className="xcn-panel-note">最多显示 100 条，使用页面搜索定位指标。</p></details>)}
      </section>
    </div>;
  }
  detail() {
    const {selected, detail} = this.state;
    if (!selected) return null;
    const type = selected.type;
    const r = type === 'sessions' && detail ? detail[0].items.find(s => s.seid === selected.row.seid) || selected.row :
      type === this.props.view && this.state.data && this.state.data.items ?
        this.state.data.items.find(s => s.supi && s.supi === selected.row.supi || s.gnb_id !== undefined && s.gnb_id === selected.row.gnb_id && s.plmn === selected.row.plmn) || selected.row : selected.row;
    const sessions = detail ? detail[0].items : [], events = detail ? detail[1].items : [];
    const id = r.supi || r.gnb_id || r.name;
    return <Drawer title={(titles[type] || '详情') + ' · ' + value(id)} onClose={this.close}>
      {r.supi && <div><div className="xcn-detail-identity"><span className="xcn-mono">{r.supi}</span><Status state={r.cm_state}/></div>
        <div className="xcn-panel-heading"><h3>速率趋势</h3>{this.windowPicker()}</div>
        <Chart theme={this.props.theme} points={detail && detail[2].points}/></div>}
      {type === 'ues' && <div><h3>接入与位置</h3><dl className="xcn-fields"><dt>位置状态</dt><dd>{value((r.location || {}).status)}</dd>
        <dt>gNB / PLMN</dt><dd className="xcn-mono">{value((r.gnb || (r.location || {}).gnb || {}).gnb_id)} / {value((r.gnb || {}).plmn)}</dd>
        <dt>小区 NCI</dt><dd className="xcn-mono">{value(((r.location || {}).nr_cgi || {}).nci_hex)}</dd></dl>
        <h3>AMF 当前 PDU 会话</h3>{(r.pdu_sessions || []).map((p, i) => <div className="xcn-detail-session" key={i}>
          <b>PSI {p.psi} / {p.dnn}</b><p className="xcn-mono">IPv4 {value(p.ipv4)}　IPv6 {value(p.ipv6)}</p><Raw data={p}/></div>)}</div>}
      {type === 'gnbs' && <div><h3>关联终端</h3>{(r.associated_ues || []).map((u, i) => <div className="xcn-detail-session" key={i}>
        <button type="button" className="xcn-link xcn-mono" onClick={() => this.select(u, 'ues')}>{u.supi}</button> <Status state={u.cm_state}/></div>)}
        <h3>观测到的小区</h3><Raw data={r.observed_cells}/>
        <Button onClick={() => this.navigate('ues', {identityFilters: {gnb_id: String(r.gnb_id), plmn: r.plmn, gnb_id_bits: String(r.gnb_id_bits)}})}>查看此 gNB 的终端</Button></div>}
      {type === 'sessions' && <div><h3>承载与规则</h3><p className="xcn-mono">PSI {value(r.psi)} / SEID {value(r.seid)} · {value(r.dnn)}</p>
        <div className="xcn-table-scroll"><table className="xcn-table"><thead><tr><th>QFI / 5QI</th><th>PDR / QER</th><th>方向</th><th>速率</th><th>累计字节</th></tr></thead>
          <tbody>{(r.rules || []).map((rule, i) => <tr key={i}><td>{value(rule.qfi)} / {value(rule['5qi'])}</td><td>{rule.pdr_id} / {rule.qer_id}</td>
            <td>{rule.direction}</td><td>{rate(rule.bps)}</td><td className="xcn-mono">{rule.bytes}</td></tr>)}</tbody></table></div><Raw data={r.bearers || []}/></div>}
      {r.supi && <div><h3>UPF 会话</h3>{sessions.length ? sessions.map(s => <div className="xcn-detail-session" key={s.seid}>
        <button type="button" className="xcn-link" onClick={() => this.select(s, 'sessions')}>PSI {value(s.psi)} / {s.dnn}</button>
        <p className="xcn-mono">SEID {s.seid}　{value(s.ue_ip)}</p><p>{rate(s.ul_bps)} ↑　{rate(s.dl_bps)} ↓</p></div>) : <Empty>暂无匹配的 UPF 会话。</Empty>}
        <h3>终端近期事件</h3><Events items={events} onSelect={e => this.select(e, 'events')}/></div>}
      <Raw data={r}/>
    </Drawer>;
  }
  render() {
    const {view} = this.props;
    const {data, error, loading, updated, page} = this.state;
    return <main className="xcn-monitor">
      <div className="xcn-page-heading"><div><h1>{titles[view]}</h1><p>{hints[view]}</p></div>
        <div className="xcn-refresh"><span className={'xcn-live-dot ' + (error ? 'warn' : '')}/>
          {typeof document !== 'undefined' && document.hidden ? '后台已暂停刷新' : '约 2 秒更新'}<small>页面更新 {updated ? clock(updated / 1000) : '等待数据'}</small></div></div>
      {error && <div className="xcn-alert warn" role="status">{error}<Button onClick={() => this.change({})}>重试</Button></div>}
      {view !== 'overview' && this.filters()}
      {view === 'sessions' && <div className="xcn-tabs" role="group" aria-label="速率层级">
        {[['user', '用户'], ['session', '会话'], ['bearer', '承载'], ['rule', '规则']].map(([k, v]) => <button key={k} type="button"
          aria-pressed={this.state.level === k} onClick={() => this.change({level: k, page: 0})}>{v}</button>)}</div>}
      {!data && <Empty>{loading ? '正在读取监控数据…' : '监控数据尚未就绪，请检查采集服务是否启用。'}</Empty>}
      {data && view === 'overview' && this.overview(data)}
      {data && view === 'system' && this.system(data)}
      {data && ['ues', 'gnbs', 'sessions', 'events'].indexOf(view) >= 0 && <section className="xcn-panel">
        {view === 'events' && data.coverage && <p className="xcn-panel-note">按容量保留，无时间过期；当前覆盖 {dateTime(data.coverage.oldest_at)} 至 {dateTime(data.coverage.newest_at)}。最多 2 万条 / 16 MiB，内存保护时可能提前淘汰。</p>}
        {view === 'events' ? <Events items={data.items} onSelect={e => this.select(e, 'events')}/> : this.table(view, data)}
        <div className="xcn-pagination"><span>共 {data.total} 条 · 第 {page + 1} 页</span><div>
          <Button disabled={page === 0} onClick={() => this.change({page: page - 1})}>上一页</Button>
          <Button disabled={(page + 1) * 50 >= data.total} onClick={() => this.change({page: page + 1})}>下一页</Button></div></div>
      </section>}
      {data && view === 'sessions' && <section className="xcn-panel"><div className="xcn-panel-heading"><h2>全局流量趋势</h2>{this.windowPicker()}</div>
        <Chart theme={this.props.theme} points={(this.state.trend || {}).points}/></section>}
      {data && data.sources && <p className="xcn-footnote">数据按来源独立采样。
        {Object.keys(data.sources).filter(k => data.sources[k].stale).length > 0 ? ' 存在陈旧数据，请查看系统状态。' : ' 查看详情可追踪关联会话与事件。'}</p>}
      {this.detail()}
    </main>;
  }
}
