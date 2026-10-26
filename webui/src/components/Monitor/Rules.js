const purposes = {
  'user-data': '用户面业务',
  'cp-to-ue': '控制面下发辅助报文',
  'ue-to-cp': '终端上送辅助报文',
  'cp-forwarding': '控制面辅助转发'
};
const interfaces = {0: '接入侧', 1: '核心侧', 2: 'N6 LAN', 3: '控制面', 4: '合法监听'};
export const ruleIdentifier = v => Number(v) > 0 ? String(v) : '—';
export const rulePurpose = rule => purposes[rule.rule_purpose] || '用途待确认';

export function Purpose({rule}) {
  const route = '来源：' + (interfaces[rule.src_if] || '未知') + '；去向：' + (interfaces[rule.dst_if] || '未知');
  return <span title={route}>{rulePurpose(rule)}</span>;
}

function Table({rules, formatRate, label}) {
  return <div className="xcn-table-scroll"><table className="xcn-table" aria-label={label}>
    <thead><tr><th scope="col">用途</th><th scope="col">QFI / 5QI</th><th scope="col">PDR / QER</th>
      <th scope="col">方向</th><th scope="col">速率</th><th scope="col">累计字节</th></tr></thead>
    <tbody>{rules.map(r => <tr key={r.pdr_id}>
      <td><Purpose rule={r}/></td><td>{ruleIdentifier(r.qfi)} / {ruleIdentifier(r['5qi'])}</td>
      <td>{ruleIdentifier(r.pdr_id)} / {ruleIdentifier(r.qer_id)}</td><td>{r.direction}</td>
      <td className="xcn-nowrap">{formatRate(r.bps)}</td><td className="xcn-mono">{r.bytes}</td>
    </tr>)}</tbody>
  </table></div>;
}

export default function Rules({rules = [], formatRate}) {
  const business = rules.filter(r => r.rule_kind === 'business');
  const internal = rules.filter(r => r.rule_kind === 'internal');
  const unknown = rules.filter(r => !['business', 'internal'].includes(r.rule_kind));
  return <div className="xcn-session-rules" role="group" aria-label="包检测规则">
    <h4>业务规则</h4>
    {business.length ? <Table rules={business} formatRate={formatRate} label="业务规则"/> :
      <p className="xcn-rule-hint">暂无已确认用途的业务规则。</p>}
    {unknown.length > 0 && <div className="xcn-rules-unknown"><h4>用途待确认</h4>
      <p className="xcn-rule-hint">接口信息尚未采集，保留展示这些规则。</p>
      <Table rules={unknown} formatRate={formatRate} label="用途待确认的规则"/></div>}
    {internal.length > 0 && <details className="xcn-rules-internal">
      <summary><span>内部辅助规则</span><span className="xcn-rule-count">{internal.length} 条</span></summary>
      <div className="xcn-rules-internal-content">
        <p className="xcn-rule-hint">用于 IPv6 路由请求、路由通告等辅助报文；无此类流量时速率为 0 正常。</p>
        <Table rules={internal} formatRate={formatRate} label="内部辅助规则"/>
      </div>
    </details>}
  </div>;
}
