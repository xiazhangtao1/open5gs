const LIMIT = 8;
const FIELDS = ['trend_key', 'trend_epoch', 'history_state', 'history_reason', 'supi', 'seid', 'psi', 'dnn',
  'ue_ip', 'qfi', '5qi', 'pdr_id', 'qer_id', 'direction', 'ul_bps', 'dl_bps', 'bps', 'session_count', 'rule_kind'];
function keyOf(row, level) {
  return row.trend_key || (level === 'user' ? 'user:' + row.supi : level === 'session' ? 'session:' + row.seid :
    level === 'bearer' ? 'bearer:' + row.seid + ':' + row.qfi : 'rule:' + row.seid + ':' + row.pdr_id);
}
function compact(row, level) {
  const result = {trend_key: keyOf(row, level)};
  FIELDS.forEach(k => { if (row[k] !== undefined) result[k] = row[k]; });
  if (row.color !== undefined) result.color = row.color;
  return result;
}
function eligible(row, level) {
  return level !== 'rule' || (row.rule_kind === 'business' && Number.isInteger(row.pdr_id) && row.pdr_id > 0);
}
function toggle(rows, row, level) {
  const key = keyOf(row, level);
  if (rows.some(r => r.trend_key === key)) return rows.filter(r => r.trend_key !== key);
  if (!eligible(row, level)) throw new Error('此规则暂不支持流量走势');
  if (rows.length >= LIMIT) throw new Error('最多选择 8 个对象，请先移除一个已选对象');
  const used = new Set(rows.map(r => r.color));
  let color = 0; while (used.has(color)) color++;
  return rows.concat({...compact(row, level), color});
}
function label(row, level, full = false) {
  const supi = row.supi || '未知终端';
  const name = full ? supi : supi.length > 12 ? '…' + supi.slice(-6) : supi;
  if (level === 'user') return name;
  const session = name + ' · PSI ' + (row.psi || '—');
  return level === 'session' ? session + ' / SEID ' + row.seid : level === 'bearer' ?
    session + ' · QFI ' + row.qfi : session + ' · PDR ' + row.pdr_id;
}
function remember(cache, name, entry) {
  const bytes = JSON.stringify(entry.data).length * 3 + 1024;
  if (bytes > 8 * 1024 * 1024) throw new Error('历史超过浏览器缓存预算');
  cache.delete(name);
  let used = 0; cache.forEach(v => { used += v.bytes || 0; });
  while (cache.size && (cache.size >= 16 || used + bytes > 8 * 1024 * 1024)) {
    const first = cache.keys().next().value; used -= cache.get(first).bytes || 0; cache.delete(first);
  }
  cache.set(name, {...entry, bytes});
}
module.exports = {LIMIT, compact, keyOf, eligible, toggle, label, remember};
