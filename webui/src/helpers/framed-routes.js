// Keep the form and subscriber model aligned with the core's per-session limit.
const MAX_IPV4_FRAMED_ROUTES = 8;
const octet = '(?:0|[1-9][0-9]?|1[0-9]{2}|2[0-4][0-9]|25[0-5])';
// The final assertion requires the actual end of input, including no newline.
const IPV4_CIDR_PATTERN = '^' + octet + '(?:\\.' + octet + '){3}' +
  '/(?:[0-9]|[12][0-9]|3[0-2])(?![\\s\\S])';

module.exports = { MAX_IPV4_FRAMED_ROUTES, IPV4_CIDR_PATTERN };
