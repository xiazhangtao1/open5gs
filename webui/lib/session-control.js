// Shared by the browser form and the control proxy. The core validates again.
const nonGbrFiveQis = [5, 6, 7, 8, 9, 10, 69, 70, 79, 80];
const units = [1000, 4000, 16000, 64000, 256000, 1000000, 4000000, 16000000,
  64000000, 256000000, 1000000000, 4000000000, 16000000000, 64000000000, 256000000000, 1000000000000];
const invalid = text => { throw Object.assign(new Error(text), {status: 400}); };
function bitrate(text) {
  const match = typeof text === 'string' && text.length <= 40 &&
    /^(\d{1,13})(?:\.(\d{1,6}))? (bps|Kbps|Mbps|Gbps|Tbps)$/.exec(text);
  if (!match) return invalid('速率格式示例：10 Mbps，最多六位小数');
  const power = ['bps', 'Kbps', 'Mbps', 'Gbps', 'Tbps'].indexOf(match[3]) * 3;
  const fraction = match[2] || '';
  if (/[1-9]/.test(fraction.slice(power))) return invalid('速率必须为整数 bit/s');
  const bps = Number(match[1] + fraction.slice(0, power).padEnd(power, '0'));
  if (!Number.isSafeInteger(bps) || bps <= 0 || bps > 4000000000000 ||
      !units.some(unit => bps % unit === 0 && bps / unit <= 65535))
    return invalid('速率超出协议范围或不能准确编码，请使用整 Kbps / Mbps 等可表示数值');
  return bps;
}
function object(value, allowed) {
  return value && typeof value === 'object' && !Array.isArray(value) &&
    Object.keys(value).every(key => allowed.includes(key));
}
function validateSessionConfig(body) {
  if (!object(body, ['defaultQos', 'sessionAmbr']) || !object(body.defaultQos, ['5qi', 'priorityLevel', 'arp']) ||
      !object(body.sessionAmbr, ['uplink', 'downlink'])) return invalid('请提交完整的默认 QoS 和 Session-AMBR 配置');
  const q = body.defaultQos, a = q.arp;
  if (!nonGbrFiveQis.includes(q['5qi'])) return invalid('默认流仅支持标准 Non-GBR 5QI');
  if (q.priorityLevel !== undefined && (!Number.isInteger(q.priorityLevel) || q.priorityLevel < 1 || q.priorityLevel > 127))
    return invalid('调度优先级范围为 1–127，留空使用 5QI 默认值');
  if (!object(a, ['priorityLevel', 'preemptionCapability', 'preemptionVulnerability']) ||
      !Number.isInteger(a.priorityLevel) || a.priorityLevel < 1 || a.priorityLevel > 15 ||
      !['MAY_PREEMPT', 'NOT_PREEMPT'].includes(a.preemptionCapability) ||
      !['PREEMPTABLE', 'NOT_PREEMPTABLE'].includes(a.preemptionVulnerability)) return invalid('ARP 参数无效');
  bitrate(body.sessionAmbr.uplink); bitrate(body.sessionAmbr.downlink);
  return body;
}
module.exports = {nonGbrFiveQis, bitrate, validateSessionConfig};
