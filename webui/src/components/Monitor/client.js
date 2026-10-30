import axios from 'axios';

let token, expires = 0, renewing;
const previous = new Map();
async function auth() {
  if (token && Date.now() < expires) return token;
  if (!renewing) renewing = axios.get('/api/auth/monitor-token', {timeout: 4000}).then(r => {
    token = r.data.token;
    expires = Date.now() + (r.data.expiresIn - 60) * 1000;
    return token;
  }).then(v => { renewing = null; return v; }, e => { renewing = null; throw e; });
  return renewing;
}
export async function get(endpoint, params = {}, cancelToken) {
  const url = '/api/monitor/' + endpoint;
  const key = url + '?' + JSON.stringify(params);
  // Trend consumers retain only their active views and cursor, not every
  // distinct incremental URL in this general response cache.
  const cacheable = endpoint !== 'signal' && endpoint !== 'trends' && endpoint !== 'trends-batch';
  const old = cacheable ? previous.get(key) : null;
  const access = await auth();
  try {
    const response = await axios.get(url, {params, cancelToken, timeout: 4000,
      headers: {Authorization: 'Bearer ' + access, ...(old ? {'If-None-Match': old.etag} : {})},
      validateStatus: s => s === 200 || s === 304});
    if (response.status === 304 && old) return old.data;
    if (cacheable) {
      if (previous.size >= 64) previous.delete(previous.keys().next().value);
      previous.set(key, {etag: response.headers.etag, data: response.data});
    }
    return response.data;
  } catch (e) {
    if (e.response && e.response.status === 401) { token = null; expires = 0; }
    throw e;
  }
}
