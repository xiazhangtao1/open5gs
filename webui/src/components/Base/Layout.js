import PropTypes from 'prop-types';
import Head from 'next/head';

import styled from 'styled-components';
import oc from 'open-color';

import Header from 'containers/Header';
import Sidebar from 'containers/Sidebar';

import Package from '../../../package';

const Body = styled.div`
  display: flex;
  height: calc(100vh - 4rem);
`

const propTypes = {
  title: PropTypes.string
}

const defaultProps = {
  title: `Open5GS ${Package.version}`
}

const Layout = ({ title, children, theme, onToggleTheme }) => (
  <div className="xcn-app" data-theme={theme}>
    <Head>
      <title>{title}</title>
    </Head>
    <Header theme={theme} onToggleTheme={onToggleTheme}/>
    <Body>
      <Sidebar/>
      {children}
    </Body>
  </div>
)

Layout.propTypes = propTypes;
Layout.defaultProps = defaultProps;

const ContainerWrapper = styled.div`
  flex: 1;
  min-width: 0;
  overflow: auto;
  background: var(--canvas);
`;

Layout.Container = ({visible, children}) => visible ? (
  <ContainerWrapper>
    {children}
  </ContainerWrapper>
) : null;

Layout.Content = styled.div`
`;

export default Layout;
