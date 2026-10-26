// Incremental history is replaced at the overlap boundary, never summed.
function mergeTrend(previous, next) {
  const merge = field => {
    const rows = !previous || next.reset || previous.epoch !== next.epoch ||
      previous.window_seconds !== next.window_seconds ? [] :
      (previous[field] || []).filter(row => row[0] >= Math.max(next.window_start,
        next.available_from === null || next.available_from === undefined ? next.window_start : next.available_from) &&
        row[0] < next.replace_from);
    const result = rows.concat(next[field] || []);
    if (result.length > 3000) throw new Error('速率历史超过展示容量');
    return result;
  };
  return {...next, points: merge('points'), buckets: merge('buckets')};
}
module.exports = {mergeTrend};
