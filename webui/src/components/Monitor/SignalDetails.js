import {Component} from 'react';
import axios from 'axios';
import {get} from './client';

const messages = {
  not_collected: '该消息仅有摘要，未采集码流。', not_retained: '该消息的详细码流未保留。',
  evicted: '详细码流已因容量限制淘汰，摘要仍可查看。', disabled: '详细信令采集已暂停。',
  malformed: '报文未能完整解析，可结合消息摘要定位。', decode_failed: '该报文暂时无法解析。',
  decode_timeout: '该报文解析超时，已停止本次任务。', output_limit: '该报文的解析内容超过展示容量。',
  queue_full: '当前查看详情的人较多，请稍后重试。'
};
const security = (flags, protocol) => [flags & 1 ? protocol === 'NAS-5GSM' ? '会话消息明文' : '未保护明文' : null, flags & 2 ? '完整性校验通过' : null,
  flags & 4 ? '已解密' : null, flags & 8 ? '完整性校验失败' : null,
  flags & 16 ? '发送前明文' : null].filter(Boolean).join(' · ') || '完整性未检查';

function filterTree(nodes, query, encoding) {
  return nodes.reduce((out, n) => {
    const children = filterTree(n.children || [], query, encoding);
    if (!encoding && n.name.startsWith('per.')) return out.concat(children);
    if (!query || [n.name, n.label, n.value].join(' ').toLowerCase().includes(query)) out.push({...n, children: query ? filterTree(n.children || [], '', encoding) : children});
    else if (children.length) out.push({...n, children});
    return out;
  }, []);
}

async function copy(text) {
  if (navigator.clipboard && window.isSecureContext) {
    try { await navigator.clipboard.writeText(text); return; } catch (error) { /* Try the HTTP-compatible path. */ }
  }
  const field = document.createElement('textarea');
  const focused = document.activeElement;
  field.value = text; field.style.position = 'fixed'; field.style.opacity = '0';
  document.body.appendChild(field); field.select();
  const ok = document.execCommand('copy');
  document.body.removeChild(field);
  if (focused && focused.focus) focused.focus();
  if (!ok) throw new Error('copy failed');
}

class TreeNode extends Component {
  state = {open: this.props.depth < 3 || this.props.depth <= 10 &&
    ((this.props.node.children || []).length === 1 ||
      ['ngap.protocolIEs', 'ngap.ProtocolIE_Field_element'].includes(this.props.node.name)), count: 100};
  render() {
    const {node, depth, searching, select} = this.props;
    const children = node.children || [];
    const open = this.state.open || searching;
    return <li className="xcn-signal-node">
      <div className={node.redacted ? 'redacted' : ''}>
        {children.length ? <button type="button" className="xcn-tree-toggle" aria-expanded={open}
          aria-label={(open ? '收起 ' : '展开 ') + node.name}
          onClick={() => this.setState({open: !open})}>{open ? '−' : '+'}</button> : <span className="xcn-tree-leaf"/>}
        <button type="button" className="xcn-tree-field" title={node.name} onClick={() => select(node)}>
          {node.label || node.name}{node.redacted && <span className="xcn-signal-masked"> 已遮蔽</span>}
        </button>
      </div>
      {!!children.length && open && <ul>{children.slice(0, this.state.count).map((n, i) =>
        <TreeNode key={i} node={n} depth={depth + 1} searching={searching} select={select}/>)}
        {children.length > this.state.count && <li><button type="button" className="xcn-link"
          onClick={() => this.setState({count: this.state.count + 100})}>继续显示字段（剩余 {children.length - this.state.count}）</button></li>}
      </ul>}
    </li>;
  }
}

export default class SignalDetails extends Component {
  state = {view: 'decoded', doc: null, hex: null, query: '', encoding: false, error: null, copied: '', page: 0, selected: null};
  componentDidMount() { this.active = true; this.started = Date.now(); this.load(); }
  componentWillUnmount() { this.active = false; clearTimeout(this.timer); if (this.cancel) this.cancel.cancel(); }
  load = async () => {
    clearTimeout(this.timer);
    if (!this.active) return;
    if (this.cancel) this.cancel.cancel();
    this.cancel = axios.CancelToken.source();
    const view = this.state.view;
    try {
      const doc = await get('signal', {id: this.props.event.event_id, view}, this.cancel.token);
      if (!this.active || view !== this.state.view) return;
      this.setState({[view === 'hex' ? 'hex' : 'doc']: doc, error: null});
      if (['queued', 'busy'].includes(doc.state) && Date.now() - this.started < 30000)
        this.timer = setTimeout(this.load, Math.max(1000, doc.retry_ms || 1000));
    } catch (e) {
      if (!this.active || axios.isCancel(e)) return;
      this.setState({error: '信令详情暂时不可用。'});
    }
  };
  retry = () => { this.started = Date.now(); this.load(); };
  view = view => { this.setState({view, error: null}, this.load); };
  select = node => {
    this.setState({selected: node, page: Math.floor((node.offset || 0) / 512)});
  };
  copy = async text => {
    try { await copy(text); if (this.active) this.setState({copied: '已复制'}); }
    catch (e) { if (this.active) this.setState({copied: '复制失败，请手动选择内容。'}); }
  };
  renderHex(doc) {
    if (!doc.hex) return <p className="xcn-signal-note">该报文未完整解码，暂不展示原始字节。</p>;
    const bytes = doc.hex.split(' '), start = this.state.page * 512;
    const selected = this.state.selected;
    const rows = [];
    for (let offset = start; offset < Math.min(bytes.length, start + 512); offset += 16) {
      rows.push(<div className="xcn-hex-row" key={offset}><span className="xcn-hex-offset">{('000000' + offset.toString(16)).slice(-6)}</span>
        <span>{bytes.slice(offset, offset + 16).map((b, i) => <span key={i} className={selected &&
          offset + i >= selected.offset && offset + i < selected.offset + selected.length ? 'selected' : b === '**' ? 'masked' : ''}>{b} </span>)}</span></div>);
    }
    return <div><div className="xcn-hex" aria-label="信令十六进制码流">{rows}</div>
      <div className="xcn-signal-actions"><button type="button" disabled={!start} onClick={() => this.setState({page: this.state.page - 1})}>上一段</button>
        <span>{Math.min(start + 1, bytes.length)}–{Math.min(start + 512, bytes.length)} / {bytes.length} 字节</span>
        <button type="button" disabled={start + 512 >= bytes.length} onClick={() => this.setState({page: this.state.page + 1})}>下一段</button>
        <button type="button" onClick={() => this.copy(doc.hex)}>复制码流</button></div></div>;
  }
  render() {
    const {view, query, selected, error} = this.state;
    const doc = view === 'hex' ? this.state.hex : this.state.doc;
    const ready = doc && doc.state === 'ready';
    const nodes = ready && view === 'decoded' ? filterTree(doc.tree || [], query.toLowerCase(), this.state.encoding) : [];
    return <section className="xcn-signal-detail" aria-label="信令内容">
      <div className="xcn-signal-heading"><h3>信令内容</h3><div className="xcn-signal-tabs">
        <button type="button" aria-pressed={view === 'decoded'} onClick={() => this.view('decoded')}>协议内容</button>
        <button type="button" aria-pressed={view === 'hex'} onClick={() => this.view('hex')}>码流</button>
      </div></div>
      {error ? <p role="status">{error} <button className="xcn-link" type="button" onClick={this.retry}>重试</button></p> :
        !doc ? <p role="status">正在读取信令内容…</p> : !ready ?
          <p role="status">{doc.state === 'queued' ? '正在解析信令，请稍候…' : messages[doc.reason] || '该消息暂无详细内容。'}
            {['queued', 'busy', 'error'].includes(doc.state) && <button type="button" className="xcn-link" onClick={this.retry}>重新读取</button>}</p> :
          <div>
            {doc.protocol !== 'NGAP' && <p className={'xcn-signal-note ' + (doc.security & 8 ? 'failure' : '')}>{security(doc.security, doc.protocol)}</p>}
            {doc.redacted && <p className="xcn-signal-note">密钥字段已遮蔽，码流中以 ** 表示。</p>}
            {view === 'decoded' ? <div>
              <input className="xcn-signal-search" aria-label="搜索协议字段" placeholder="搜索字段名或内容" value={query}
                onChange={e => this.setState({query: e.target.value})}/>
              <label className="xcn-signal-encoding"><input type="checkbox" checked={this.state.encoding}
                onChange={e => this.setState({encoding: e.target.checked})}/>显示 PER 编码细节</label>
              {nodes.length ? <ul className="xcn-signal-tree">{nodes.map((node, i) =>
                <TreeNode key={query + '/' + this.state.encoding + '/' + i} node={node} depth={0} searching={!!query} select={this.select}/>)}</ul> : <p>未找到匹配字段。</p>}
              <div className="xcn-signal-actions"><button type="button" onClick={() => this.copy(JSON.stringify(doc, null, 2))}>复制解析内容</button></div>
            </div> : this.renderHex(doc)}
            {selected && <div className="xcn-selected-field"><b>{selected.name}</b><span>{selected.value || selected.label}</span>
              <button type="button" className="xcn-link" onClick={() => this.copy(selected.value || selected.label)}>复制字段</button>
              {selected.offset !== undefined && view === 'decoded' && <button type="button" className="xcn-link" onClick={() => this.view('hex')}>查看对应字节</button>}
            </div>}
          </div>}
      {this.state.copied && <p role="status" className="xcn-signal-note">{this.state.copied}</p>}
    </section>;
  }
}
