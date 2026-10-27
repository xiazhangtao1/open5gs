import DeploymentVersion from './DeploymentVersion';

export default function Header({onSidebarToggle, onLogoutRequest, theme, onToggleTheme}) {
  return <header className="xcn-header">
    <button type="button" className="xcn-icon-button" aria-label="展开或收起导航" onClick={onSidebarToggle}>
      <svg viewBox="0 0 24 24" aria-hidden="true"><path d="M3 6h18M3 12h18M3 18h18"/></svg>
    </button>
    <div className="xcn-brand"><b>X-CN</b><div className="xcn-brand-caption">
      <span className="xcn-brand-title">核心网控制台</span>
      <DeploymentVersion/>
    </div></div>
    <div className="xcn-header-actions">
      <button type="button" className="xcn-quiet-button" onClick={onToggleTheme}
        aria-label={theme === 'dark' ? '切换浅色主题' : '切换深色主题'}>{theme === 'dark' ? '浅色' : '深色'}</button>
      <button type="button" className="xcn-quiet-button" onClick={onLogoutRequest}>退出登录</button>
    </div>
  </header>;
}
