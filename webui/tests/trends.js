const assert = require('assert');
const {mergeTrend} = require('../lib/trends');
const old = {epoch: 'a', window_seconds: 86400,
  points: [[0, 1, 2], [60, 3, 4], [120, 5, 6]], buckets: [[60, 3, 4, 58, 29]]};
const next = {epoch: 'a', window_seconds: 86400, window_start: 60, replace_from: 60,
  points: [[60, 30, 40], [120, 5, 6], [180, 7, 8]], buckets: [[60, 30, 40, 60, 30]]};
assert.deepStrictEqual(mergeTrend(old, next).points, next.points);
assert.deepStrictEqual(mergeTrend(old, next).buckets, next.buckets);
assert.deepStrictEqual(mergeTrend(old, {...next, reset: true, points: []}).points, []);
assert.deepStrictEqual(mergeTrend(old, {...next, epoch: 'b', points: []}).points, []);
assert.deepStrictEqual(mergeTrend(old, {...next, window_seconds: 1800, points: []}).points, []);
assert.deepStrictEqual(mergeTrend(old, {...next, replace_from: 180, available_from: 120,
  points: [[180, 7, 8]]}).points, [[120, 5, 6], [180, 7, 8]]);
assert.throws(() => mergeTrend(null, {...next, points: Array(3001).fill([1, 2, 3])}));
console.log('Trend overlap replacement, window pruning, epoch reset and client capacity: OK');
