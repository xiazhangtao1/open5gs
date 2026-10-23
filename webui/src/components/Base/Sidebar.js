import styled from 'styled-components';
import { media } from 'helpers/style-utils';

const Menu = styled.nav`
  flex-shrink: 0;
  width: ${p => p.visible ? '15rem' : '0'};
  overflow: hidden;
  background: var(--surface);
  border-right: 1px solid var(--line);
  z-index: 4;
  ${media.mobile`
    position: absolute;
    top: 4rem;
    left: 0;
    bottom: 0;
    width: ${p => p.visible ? '15rem' : '0'};
    box-shadow: ${p => p.visible ? '8px 0 24px rgba(0,0,0,.15)' : 'none'};
  `}
`;
const paths = {
  overview: 'M3 3h7v7H3z M14 3h7v7h-7z M3 14h7v7H3z M14 14h7v7h-7z',
  ues: 'M8 2h8v20H8z M10 18h4',
  gnbs: 'M12 8v14 M7 22h10 M8 12a6 6 0 0 1 0-8 M16 4a6 6 0 0 1 0 8 M4 16a12 12 0 0 1 0-16 M20 0a12 12 0 0 1 0 16',
  sessions: 'M2 8h18 M16 4l4 4-4 4 M22 16H4 M8 12l-4 4 4 4',
  events: 'M4 3v18 M4 6h16 M4 12h12 M4 18h16',
  system: 'M3 3h18v7H3z M3 14h18v7H3z M6 6h1 M6 17h1',
  subscriber: 'M12 12a4 4 0 1 0 0-8 4 4 0 0 0 0 8 M4 22v-3a8 5 0 0 1 16 0v3',
  profile: 'M5 2h10l4 4v16H5z M8 11h8 M8 16h8',
  account: 'M15 14a5 5 0 1 0-4-4 L2 19v3h3v-3h3l3-3'
};
const monitor = [['overview', '概览'], ['ues', '终端'], ['gnbs', '基站'],
  ['sessions', '会话'], ['events', '事件'], ['system', '状态']];
const manage = [['subscriber', '用户管理'], ['profile', '模板管理'], ['account', '账户管理']];

export default function Sidebar({isOpen, selectedView, onSelectView}) {
  const item = ([name, label]) => (
    <button key={name} type="button" className={'xcn-nav-item ' + (selectedView === name ? 'selected' : '')}
      tabIndex={isOpen ? 0 : -1} aria-current={selectedView === name ? 'page' : undefined} onClick={() => onSelectView(name)}>
      <svg viewBox="0 0 24 24" aria-hidden="true"><path d={paths[name]}/></svg><span>{label}</span>
    </button>
  );
  return <Menu visible={isOpen} aria-label="主导航" aria-hidden={!isOpen}>
    <div className="xcn-nav-group">核心网监控</div>{monitor.map(item)}
    <div className="xcn-nav-group manage">配置管理</div>{manage.map(item)}
  </Menu>;
}
