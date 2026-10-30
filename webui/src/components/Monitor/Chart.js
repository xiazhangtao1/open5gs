import {Component} from 'react';
import {label} from '../../../lib/comparison';
const escape = text => String(text).replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
const historyMessage = (trend, noun = '承载') => trend.history_state === 'released' ? noun + '已释放，显示保留的历史' :
  trend.history_state === 'unavailable' ? ({capacity: '历史容量已满，实时速率仍可查看', memory: '内存保护已暂停历史',
    identity: '缺少可靠实例标识，暂不记录历史', absent: '对象不存在或历史已淘汰', internal: '内部辅助规则不支持走势',
    unclassified: '规则用途待确认，暂不记录历史', instance: '原对象已释放，标识已被其他用户复用'}[trend.history_reason] || '历史暂不可用') : null;

let loading;
function charts() {
  if (window.echarts) return Promise.resolve(window.echarts);
  if (!loading) loading = new Promise((resolve, reject) => {
    const script = document.createElement('script');
    script.src = '/static/vendor/echarts.min.js';
    script.onload = () => resolve(window.echarts);
    script.onerror = () => { loading = null; reject(new Error('图表资源加载失败')); };
    document.head.appendChild(script);
  });
  return loading;
}

export default class Chart extends Component {
  state = {smooth: true, direction: 'all'};
  componentDidMount() {
    this.active = true;
    charts().then(ec => {
      if (!this.active) return;
      this.chart = ec.init(this.element, null, {renderer: 'svg'});
      this.draw();
      window.addEventListener('resize', this.resize);
      if (window.ResizeObserver) {
        this.observer = new ResizeObserver(this.resize);
        this.observer.observe(this.element);
      }
    }).catch(() => { if (this.active) this.setState({error: true}); });
  }
  componentDidUpdate() { this.draw(); }
  componentWillUnmount() {
    this.active = false;
    window.removeEventListener('resize', this.resize);
    if (this.observer) this.observer.disconnect();
    if (this.chart) this.chart.dispose();
  }
  resize = () => { if (this.chart) this.chart.resize(); };
  draw() {
    if (!this.chart) return;
    const trend = this.props.trend || ((this.props.comparisons || [])[0] || {}).trend || {};
    const points = trend.points || this.props.points || [];
    const minute = trend.aggregation === 'minute';
    const dark = this.props.theme === 'dark';
    const colors = dark ? ['#74a7ff','#4bc8a2','#f5bf66','#c7a0ff','#f59bc0','#67d0e4','#ffad87','#c3cedd'] :
      ['#2563eb','#16805d','#b45309','#9333ea','#be185d','#0e7490','#c2410c','#475569'];
    const items = this.props.comparisons || [{trend: {...trend, points}, row: {}, single: true}];
    const lines = [];
    items.forEach(item => ['UL', 'DL'].forEach((direction, i) => {
      if (this.props.level === 'rule' && item.row.direction !== direction) return;
      if (this.state.direction !== 'all' && this.state.direction !== direction) return;
      lines.push({item, direction, i, name: (item.single ? '' : label(item.row, this.props.level) + ' ') + (i ? '下行' : '上行'),
        id: (item.row.trend_key || 'single') + ':' + direction,
        color: item.single ? (i ? dark ? '#4bc8a2' : '#16805d' : '#2563eb') : colors[item.row.color % 8]});
    }));
    const previousLegend = ((this.chart.getOption() || {}).legend || [])[0] || {};
    const selected = {};
    lines.forEach(line => {
      if (previousLegend.selected && previousLegend.selected[line.id] !== undefined) selected[line.id] = previousLegend.selected[line.id];
    });
    this.chart.setOption({
      animation: false,
      color: ['#2563EB', dark ? '#4BC8A2' : '#16805D'],
      grid: {left: 58, right: 18, top: 58, bottom: minute ? 46 : 32},
      legend: {type: 'scroll', selected, left: 12, right: 110, top: 0, data: lines.map(line => line.id), formatter: name => (lines.find(line => line.id === name) || {}).name || name,
        textStyle: {color: dark ? '#CDD6E4' : '#526176'}, pageTextStyle: {color: dark ? '#CDD6E4' : '#526176'}},
      tooltip: {trigger: 'axis', confine: true, formatter: rows => {
        if (!rows.length) return '';
        const stamp = rows[0].value[0] / 1000;
        return new Date(stamp * 1000).toLocaleString('zh-CN', {hour12: false}) + (minute ? ' · 分钟汇总' : '') + '<br/>' +
          rows.map(r => {
            const line = lines[r.seriesIndex];
            const bucket = (line.item.trend.buckets || []).find(b => b[0] === stamp);
            return escape(line.item.single ? line.name : label(line.item.row, this.props.level, true) + ' ' + (line.i ? '下行' : '上行')) +
              '：' + (r.value[1] === null ? '未采集' : Number(r.value[1]).toFixed(3) + ' Mbps') +
              (minute && bucket ? ' · 峰值 ' + (bucket[line.i + 1] / 1e6).toFixed(3) + ' Mbps · 覆盖 ' + bucket[3].toFixed(1) + '/60 秒' : '');
          }).join('<br/>');
      }},
      xAxis: {type: 'time', axisLine: {lineStyle: {color: dark ? '#41516B' : '#DAE0E8'}},
        ...(trend.window_start !== undefined ? {min: trend.window_start * 1000, max: trend.window_end * 1000} : {}),
        axisLabel: {color: dark ? '#A8B6CB' : '#64748B', hideOverlap: true,
          ...(minute ? {formatter: value => {
            const date = new Date(value), two = n => ('0' + n).slice(-2);
            return two(date.getMonth() + 1) + '/' + two(date.getDate()) + '\n' + two(date.getHours()) + ':' + two(date.getMinutes());
          }} : {})}},
      yAxis: {type: 'value', name: 'Mbps', min: 0, nameTextStyle: {color: dark ? '#A8B6CB' : '#64748B'},
        axisLabel: {color: dark ? '#A8B6CB' : '#64748B'},
        splitLine: {lineStyle: {color: dark ? '#2B3B53' : '#E8EDF3'}}},
      series: lines.map(line => {
        const data = line.item.trend.points || [];
        const buckets = new Map((line.item.trend.buckets || []).map(b => [b[0], b]));
        return {name: line.id, type: 'line', showSymbol: minute || data.length < 2,
          itemStyle: {color: line.color}, lineStyle: {width: 2, color: line.color, type: line.direction === 'UL' ? 'dashed' : 'solid'},
          symbolSize: minute ? value => {const b = buckets.get(value[0] / 1000);return b && b[3] < 59.999 ? 5 : 0;} : 6,
          connectNulls: false, smooth: this.state.smooth ? 0.25 : false, smoothMonotone: 'x',
          data: data.map(p => [p[0] * 1000, p[line.i + 1] === null ? null : p[line.i + 1] / 1e6])};
      })
    }, true);
  }
  render() {
    const trend = this.props.trend || ((this.props.comparisons || [])[0] || {}).trend || {};
    const comparisons = this.props.comparisons;
    const empty = comparisons ? !comparisons.some(item => ['UL', 'DL'].some((direction, i) =>
      (this.state.direction === 'all' || this.state.direction === direction) &&
      (this.props.level !== 'rule' || item.row.direction === direction) &&
      (item.trend.points || []).some(p => p[i + 1] !== null))) :
      !(trend.points || this.props.points || []).some(p => p[1] !== null);
    const message = comparisons ? null : historyMessage(trend);
    const failed = this.state && this.state.error;
    const date = t => new Date(t * 1000).toLocaleString('zh-CN', {hour12: false});
    return <div className="xcn-chart-wrap">
      <div className="xcn-chart-mode">
        {comparisons && <select aria-label="对比流量方向" value={this.state.direction}
          onChange={e => this.setState({direction: e.target.value})}>
          <option value="all">上下行</option><option value="UL">上行</option><option value="DL">下行</option></select>}
        <select aria-label="速率显示方式" value={this.state.smooth ? 'smooth' : 'raw'}
          title="仅切换折线与曲线，采样值不变"
          onChange={event => this.setState({smooth: event.target.value === 'smooth'})}>
          <option value="raw">折线</option>
          <option value="smooth">曲线</option>
        </select>
      </div>
      <div ref={e => { this.element = e; }} className="xcn-chart" role="img" aria-label="上行与下行速率走势图"/>
      {(empty || failed) && <div className="xcn-chart-empty">{failed ? '图表资源加载失败，请刷新重试' : message || '等待流量采样，暂时没有历史数据'}</div>}
      <div className="xcn-chart-caption" role="status">
        <span>{trend.aggregation === 'minute' ? '分钟汇总 · 每分钟更新' : '原始采样'}</span>
        {trend.available_from !== undefined && trend.available_from !== null &&
          <span>已有历史 {date(trend.available_from)} — {date(trend.available_to)}</span>}
        {message && <span className="xcn-chart-warning">{message}</span>}
        {comparisons && comparisons.map(item => {
          const note = historyMessage(item.trend, this.props.level === 'rule' ? '规则' : '对象') || item.trend.error;
          return note ? <span className="xcn-chart-warning" key={item.row.trend_key}>{label(item.row, this.props.level)}：{note}</span> : null;
        })}
        {trend.error && <span className="xcn-chart-warning">{trend.error}</span>}
      </div>
    </div>;
  }
}
