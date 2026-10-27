import {Component} from 'react';
import axios from 'axios';

export default class DeploymentVersion extends Component {
  state = {version: null, appVersion: null};

  componentDidMount() {
    this.active = true;
    this.request = axios.CancelToken.source();
    axios.get('/api/version', {timeout: 3000, cancelToken: this.request.token}).then(({data}) => {
      if (!this.active || !data) return;
      this.setState({
        version: typeof data.version === 'string' ? data.version : null,
        appVersion: typeof data.appVersion === 'string' ? data.appVersion : null
      });
    }).catch(() => {});
  }

  componentWillUnmount() {
    this.active = false;
    this.request.cancel();
  }

  render() {
    const {version, appVersion} = this.state;
    if (!version && !appVersion) return null;
    return <small className="xcn-deployment-version" role="group" aria-label="部署版本">
      <span title={'Chart.yaml version: ' + (version || '未配置')}>Chart {version || '—'}</span>
      <span title={'Chart.yaml appVersion: ' + (appVersion || '未配置')}>App {appVersion || '—'}</span>
    </small>;
  }
}
