// Bundle the pinned chart asset locally; the browser never needs a CDN.
const fs = require('fs');
const path = require('path');
const target = path.join(__dirname, '../static/vendor');
fs.mkdirSync(target, {recursive: true});
fs.copyFileSync(require.resolve('echarts/dist/echarts.min.js'), path.join(target, 'echarts.min.js'));
fs.copyFileSync(require.resolve('echarts/LICENSE'), path.join(target, 'echarts.LICENSE'));
