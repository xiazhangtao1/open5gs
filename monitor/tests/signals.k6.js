// Isolated signal fixture deployment only. SAME_ID / ID_PREFIX select known retained messages.
import http from 'k6/http';
import {check, sleep} from 'k6';
import {Rate, Trend} from 'k6/metrics';
const latency = new Trend('signal_latency', true);
const cold = new Trend('signal_first_ready', true);
const errors = new Rate('signal_errors');
export const options = {
  noCookiesReset: true,
  scenarios: {readers: {executor: 'constant-vus', vus: Number(__ENV.VUS || 50), duration: __ENV.DURATION || '1m'}},
  thresholds: {signal_latency: ['p(95)<500', 'p(99)<1500'], signal_first_ready: ['p(95)<30000'], signal_errors: ['rate==0'], checks: ['rate==1']},
  summaryTrendStats: ['avg', 'min', 'med', 'max', 'p(95)', 'p(99)']
};
const base = __ENV.BASE_URL || 'http://127.0.0.1:19999';
const prefix = __ENV.ID_PREFIX || 'AMF/00000000-0000-0000-0000-000000000001/';
let token, renewAt = 0, started, first = false;
function auth() {
  if (!token) {
    const csrf = http.get(base + '/api/auth/csrf').json('csrfToken');
    const response = http.post(base + '/api/auth/login', {username: __ENV.USERNAME, password: __ENV.PASSWORD},
      {redirects: 0, headers: {'X-CSRF-TOKEN': csrf}, tags: {name: 'login'}});
    if (!check(response, {'login succeeds': r => r.status === 302})) throw new Error('login failed');
  }
  if (Date.now() >= renewAt) {
    const response = http.get(base + '/api/auth/monitor-token', {tags: {name: 'renew'}});
    if (!check(response, {'token issued': r => r.status === 200})) throw new Error('token unavailable');
    token = response.json('token'); renewAt = Date.now() + 240000;
  }
}
export default function () {
  try {
    auth();
    if (!started) started = Date.now();
    const id = __ENV.SAME_ID || prefix + (__VU * 10 + 1);
    const response = http.get(base + '/api/monitor/signal?id=' + encodeURIComponent(id) + '&view=decoded',
      {headers: {Authorization: 'Bearer ' + token}, tags: {name: 'signal'}});
    latency.add(response.timings.duration);
    const ok = check(response, {'signal API succeeds': r => r.status === 200,
      'state is ready or bounded queue': r => r.status === 200 && ['ready', 'queued', 'busy'].includes(r.json('state'))});
    errors.add(!ok);
    if (ok && response.json('state') === 'ready' && !first) { cold.add(Date.now() - started); first = true; }
    if (!first && Date.now() - started > 30000) { errors.add(true); throw new Error('signal did not become ready'); }
  } catch (error) { errors.add(true); }
  sleep(1);
}
