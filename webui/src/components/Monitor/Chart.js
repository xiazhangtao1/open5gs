import {Component} from 'react';

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
  state = {smooth: true};
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
    const points = this.props.points || [];
    const dark = this.props.theme === 'dark';
    this.chart.setOption({
      animation: false,
      color: ['#2563EB', dark ? '#4BC8A2' : '#16805D'],
      grid: {left: 58, right: 18, top: 58, bottom: 32},
      legend: {left: 12, top: 0, data: ['上行', '下行'], textStyle: {color: dark ? '#CDD6E4' : '#526176'}},
      tooltip: {trigger: 'axis', confine: true, valueFormatter: value => Number(value).toFixed(3) + ' Mbps'},
      xAxis: {type: 'time', axisLine: {lineStyle: {color: dark ? '#41516B' : '#DAE0E8'}},
        axisLabel: {color: dark ? '#A8B6CB' : '#64748B', hideOverlap: true}},
      yAxis: {type: 'value', name: 'Mbps', min: 0, nameTextStyle: {color: dark ? '#A8B6CB' : '#64748B'},
        axisLabel: {color: dark ? '#A8B6CB' : '#64748B'},
        splitLine: {lineStyle: {color: dark ? '#2B3B53' : '#E8EDF3'}}},
      series: ['上行', '下行'].map((name, i) => ({name, type: 'line', showSymbol: points.length < 2,
        symbolSize: 6, connectNulls: false, lineStyle: {width: 2},
        smooth: this.state.smooth ? 0.25 : false, smoothMonotone: 'x',
        data: points.map(p => [p[0] * 1000, p[i + 1] === null ? null : p[i + 1] / 1000000])}))
    }, true);
  }
  render() {
    const empty = !(this.props.points || []).length;
    const failed = this.state && this.state.error;
    return <div className="xcn-chart-wrap">
      <div className="xcn-chart-mode">
        <select aria-label="速率显示方式" value={this.state.smooth ? 'smooth' : 'raw'}
          title="仅切换折线与曲线，采样值不变"
          onChange={event => this.setState({smooth: event.target.value === 'smooth'})}>
          <option value="raw">折线</option>
          <option value="smooth">曲线</option>
        </select>
      </div>
      <div ref={e => { this.element = e; }} className="xcn-chart" role="img" aria-label="上行与下行速率趋势图"/>
      {(empty || failed) && <div className="xcn-chart-empty">{failed ? '图表资源加载失败，请刷新重试' : '等待流量采样，暂时没有历史数据'}</div>}
    </div>;
  }
}
