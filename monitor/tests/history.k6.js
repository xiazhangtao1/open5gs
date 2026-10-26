// Real login + monitor proxy; only GETs after authentication. Test credentials
// must refer to a dedicated account in an isolated/authorized deployment.
import http from 'k6/http';
import {check, sleep} from 'k6';
import {Rate, Trend} from 'k6/metrics';

const errors = new Rate('history_errors');
const latency = new Trend('history_latency', true);
export const options = {
  noCookiesReset: true,
  scenarios: {readers: {executor: 'constant-vus', vus: Number(__ENV.VUS || 50), duration: __ENV.DURATION || '30m'}},
  thresholds: {history_errors: ['rate==0'], history_latency: ['p(95)<300', 'p(99)<1000'], checks: ['rate==1']}
};
const base = __ENV.BASE_URL || 'http://127.0.0.1:29999';
let token, renewed = 0, selected, nextTrend = 0;
const cursors = {};

function get(path) {
  const response = http.get(base + '/api/monitor/' + path, {headers: {Authorization: 'Bearer ' + token},
    tags: {name: path.split('?')[0]}});
  const ok = check(response, {'history request succeeds': r => r.status === 200});
  errors.add(!ok); latency.add(response.timings.duration);
  return ok ? response.json() : null;
}

export default function () {
  const begin = Date.now();
  if (!token) {
    const csrf = http.get(base + '/api/auth/csrf').json('csrfToken');
    const login = http.post(base + '/api/auth/login', {username: __ENV.USERNAME, password: __ENV.PASSWORD},
      {redirects: 0, headers: {'X-CSRF-TOKEN': csrf}});
    if (!check(login, {'login succeeds': r => r.status === 302})) { errors.add(true); sleep(2); return; }
  }
  if (Date.now() >= renewed) {
    const access = http.get(base + '/api/auth/monitor-token');
    if (!check(access, {'token succeeds': r => r.status === 200})) { errors.add(true); sleep(2); return; }
    token = access.json('token'); renewed = Date.now() + 240000;
  }
  const rates = get('rates?limit=50&page=' + (__VU % 4));
  if (!selected && rates && rates.items.length) {
    const row = rates.items[__VU % rates.items.length];
    selected = __VU % 2 ? 'user:' + row.supi : 'session:' + row.seid;
  }
  const seconds = [1800, 43200, 86400][__VU % 3];
  if (Date.now() >= nextTrend) {
    for (const key of selected ? ['global', selected] : ['global']) {
      const id = key + '/' + seconds;
      const trend = get('trends?key=' + encodeURIComponent(key) + '&seconds=' + seconds +
        (cursors[id] ? '&cursor=' + encodeURIComponent(cursors[id]) : ''));
      if (trend) {
        const valid = check(trend, {
          'window preserved': d => d.window_seconds === seconds,
          'response rows bounded': d => d.points.length <= 3000 && d.buckets.length <= 1440,
          'resolution selected': d => d.aggregation === (seconds > 1800 ? 'minute' : 'raw')
        });
        errors.add(!valid); cursors[id] = trend.cursor;
      }
    }
    nextTrend = Date.now() + (seconds > 1800 ? 60000 : 2000);
  }
  sleep(Math.max(.01, 2 - (Date.now() - begin) / 1000) + Math.random() * .2);
}
