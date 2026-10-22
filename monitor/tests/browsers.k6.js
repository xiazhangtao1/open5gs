// Run against an isolated WebUI or an approved performance test deployment.
// k6 run -e BASE_URL=http://127.0.0.1:19999 -e USERNAME=... -e PASSWORD=... browsers.k6.js
import http from 'k6/http';
import {check, sleep} from 'k6';
import {Rate, Trend} from 'k6/metrics';

const latency = new Trend('monitor_latency', true);
const errors = new Rate('monitor_errors');
export const options = {
  noCookiesReset: true,
  scenarios: {browsers: {executor: 'constant-vus', vus: Number(__ENV.VUS || 50), duration: __ENV.DURATION || '30m'}},
  thresholds: {monitor_latency: ['p(95)<300', 'p(99)<1000'], monitor_errors: ['rate==0'], checks: ['rate==1']},
  summaryTrendStats: ['avg', 'min', 'med', 'max', 'p(95)', 'p(99)']
};
const base = __ENV.BASE_URL || 'http://127.0.0.1:19999';
let token, renewAt = 0;
const etags = {};

function authenticate() {
  if (!token) {
    const csrf = http.get(base + '/api/auth/csrf').json('csrfToken');
    const login = http.post(base + '/api/auth/login', {username: __ENV.USERNAME, password: __ENV.PASSWORD}, {
      redirects: 0, headers: {'X-CSRF-TOKEN': csrf}, tags: {name: 'login'}
    });
    if (!check(login, {'real login succeeds': r => r.status === 302})) throw new Error('login failed');
  }
  if (Date.now() >= renewAt) {
    const response = http.get(base + '/api/auth/monitor-token', {tags: {name: 'renew'}});
    if (!check(response, {'monitor token issued': r => r.status === 200})) throw new Error('token failed');
    token = response.json('token');
    renewAt = Date.now() + 240000;
  }
}

export default function () {
  try { authenticate(); } catch (err) { errors.add(true); sleep(2); return; }
  const begin = Date.now();
  const pages = [
    ['overview', 'trends?seconds=1800'], ['ues?page=0&limit=50'], ['gnbs'],
    ['rates?level=' + ['user', 'session', 'bearer', 'rule'][__VU % 4] + '&limit=50', 'trends?seconds=300'],
    ['events?limit=50' + (__VU % 2 ? '&nf=amf' : '')], ['system']
  ][__VU % 6];
  const requests = pages.map(path => ({method: 'GET', url: base + '/api/monitor/' + path,
    params: {headers: {Authorization: 'Bearer ' + token, ...(etags[path] ? {'If-None-Match': etags[path]} : {})},
      tags: {name: path.split('?')[0]}}}));
  const responses = http.batch(requests);
  responses.forEach((response, i) => {
    const ok = check(response, {'monitor returns 200/304': r => r.status === 200 || r.status === 304});
    errors.add(!ok);
    latency.add(response.timings.duration);
    if (response.headers.Etag) etags[pages[i]] = response.headers.Etag;
  });
  sleep(Math.max(0, 2 - (Date.now() - begin) / 1000) + Math.random() * .2);
}
