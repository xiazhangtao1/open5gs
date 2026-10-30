import {Component} from 'react';
import axios from 'axios';
import {get} from './client';
import {keyOf, eligible, toggle, label} from '../../../lib/comparison';

export default class ComparisonPicker extends Component {
  state = {rows: this.props.rows.slice(), items: [], total: 0, page: 0, search: '', q: '', loading: true};
  componentDidMount() { this.active = true; this.origin = document.activeElement; this.panel.focus(); this.load(); }
  componentWillUnmount() { this.active = false; if (this.cancel) this.cancel.cancel(); if (this.origin && document.contains(this.origin)) this.origin.focus(); }
  load = async () => {
    if (this.cancel) this.cancel.cancel();
    this.cancel = axios.CancelToken.source();
    this.setState({loading: true, error: null});
    try {
      const data = await get('rates', {level: this.props.level, rule_scope: 'business', q: this.state.q,
        page: this.state.page, limit: 50, purpose: 'selection'}, this.cancel.token);
      if (this.active) this.setState({items: data.items, total: data.total, loading: false});
    } catch (e) { if (this.active && !axios.isCancel(e)) this.setState({error: '读取候选对象失败，请重试', loading: false}); }
  };
  choose = row => {
    try { this.setState({rows: toggle(this.state.rows, row, this.props.level), error: null}); }
    catch (e) { this.setState({error: e.message}); }
  };
  keys = e => {
    if (e.key === 'Escape') this.props.onClose();
    if (e.key !== 'Tab') return;
    const nodes = Array.from(this.panel.querySelectorAll('button,input')).filter(n => !n.disabled && n.getClientRects().length);
    const first = nodes[0], last = nodes[nodes.length - 1];
    if (e.shiftKey && document.activeElement === first) {last.focus();e.preventDefault();}
    if (!e.shiftKey && document.activeElement === last) {first.focus();e.preventDefault();}
  };
  render() {
    const {level} = this.props, s = this.state;
    return <div className="xcn-drawer-backdrop xcn-comparison-backdrop" onClick={e => {if (e.target === e.currentTarget) this.props.onClose();}}>
      <section role="dialog" aria-modal="true" aria-labelledby="comparison-title" className="xcn-comparison-picker"
        tabIndex="-1" ref={e => {this.panel = e;}} onKeyDown={this.keys}>
        <header><h2 id="comparison-title">选择对比</h2><button className="xcn-button" type="button" onClick={this.props.onClose}>取消</button></header>
        <form onSubmit={e => {e.preventDefault();this.setState({q: s.search.trim(), page: 0}, this.load);}}>
          <input aria-label="搜索对比对象" placeholder="搜索 SUPI、IP、DNN、标识…" value={s.search} onChange={e => this.setState({search: e.target.value})}/>
          <button type="submit" className="xcn-button">搜索</button></form>
        <p className="xcn-comparison-count">已选 {s.rows.length} / 8 · 可跨页选择</p>
        {s.error && <p role="alert" className="xcn-chart-warning">{s.error}</p>}
        <div className="xcn-comparison-chips">{s.rows.map(row => <button type="button" className="xcn-comparison-chip" key={row.trend_key}
          onClick={() => this.choose(row)} aria-label={'移除 ' + label(row, level, true)}>{label(row, level)} ×</button>)}</div>
        <div className="xcn-picker-items" aria-busy={s.loading}>{s.items.map(row => <label key={keyOf(row, level)}>
          <input type="checkbox" disabled={!eligible(row, level)} checked={s.rows.some(r => r.trend_key === keyOf(row, level))}
            onChange={() => this.choose(row)}/><span>{label(row, level, true)}<small>{row.dnn || '—'} · {row.ue_ip || '—'}
              {level === 'rule' ? ' · ' + row.direction : ''}{!eligible(row, level) ? ' · 暂不支持走势' : ''}</small></span></label>)}
          {!s.items.length && <p>{s.loading ? '正在读取…' : '暂无匹配对象'}</p>}</div>
        <footer><span>共 {s.total} 项</span><button className="xcn-button" type="button" disabled={!s.page || s.loading}
          onClick={() => this.setState({page: s.page - 1}, this.load)}>上一页</button>
          <button className="xcn-button" type="button" disabled={(s.page + 1) * 50 >= s.total || s.loading}
            onClick={() => this.setState({page: s.page + 1}, this.load)}>下一页</button>
          <button className="xcn-button primary" type="button" onClick={() => this.props.onApply(s.rows)}>应用</button></footer>
      </section></div>;
  }
}
