import { Component } from 'react';
import PropTypes from 'prop-types';
import { connect } from 'react-redux';
import { bindActionCreators, compose } from 'redux';

import * as sidebarActions from 'modules/sidebar';
import withWidth, { SMALL } from 'helpers/with-width';

import { Layout } from 'components';
import Notification from 'containers/Notification';
import * as Subscriber from 'containers/Subscriber';
import * as Profile from 'containers/Profile';
import * as Account from 'containers/Account';
import Monitor from 'components/Monitor';

class App extends Component {
  state = {theme: 'light'};

  componentDidMount() {
    try {
      this.setState({theme: localStorage.getItem('xcn-theme') ||
        (window.matchMedia('(prefers-color-scheme: dark)').matches ? 'dark' : 'light')});
    } catch (err) {}
  }

  toggleTheme = () => {
    const theme = this.state.theme === 'dark' ? 'light' : 'dark';
    this.setState({theme});
    try { localStorage.setItem('xcn-theme', theme); } catch (err) {}
  };
  static propTypes = {
    session: PropTypes.object.isRequired,
    view: PropTypes.string.isRequired,
    width: PropTypes.number.isRequired
  }
  
  componentWillMount() {
    const { 
      width,
      SidebarActions
    } = this.props;

    if (width !== SMALL) {
      SidebarActions.setVisibility(true);
    }
  }

  render() {
    const {
      view,
      session
    } = this.props;

    return (
      <Layout title="X-CN 核心网控制台" theme={this.state.theme} onToggleTheme={this.toggleTheme}>
        <Layout.Container visible={['overview', 'ues', 'gnbs', 'sessions', 'events', 'system'].indexOf(view) >= 0}>
          <Monitor session={session} view={view} theme={this.state.theme} onNavigate={this.props.SidebarActions.selectView}/>
        </Layout.Container>
        <Layout.Container visible={view === "subscriber"}>
          <Subscriber.Collection/>
        </Layout.Container>
        <Layout.Container visible={view === "profile"}>
          <Profile.Collection/>
        </Layout.Container>
        <Layout.Container visible={view === "account"}>
          <Account.Collection session={session}/>
        </Layout.Container>
        <Notification/>
      </Layout>
    )
  }
}

const enhance = compose(
  withWidth(),
  connect(
    (state) => ({
      view: state.sidebar.view
    }),
    (dispatch) => ({
      SidebarActions: bindActionCreators(sidebarActions, dispatch)
    })
  )
);

export default enhance(App);
