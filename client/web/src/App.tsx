import { useEffect } from 'react';
import { useAuthStore } from './store/authStore';
import { useStickyStore } from './store/stickyStore';
import { useUiStore } from './store/uiStore';
import LoginView from './views/LoginView';
import StickyBoard from './views/StickyBoard';
import AppBar from './components/AppBar';

export default function App() {
  const token = useAuthStore((s) => s.token);
  const hydrateCheckExpiry = useAuthStore((s) => s.hydrateCheckExpiry);
  const ensureDefault = useStickyStore((s) => s.ensureDefault);
  const darkMode = useUiStore((s) => s.darkMode);

  // 启动时检查 token 过期
  useEffect(() => {
    hydrateCheckExpiry();
  }, [hydrateCheckExpiry]);

  // 已登录确保至少一张便签
  useEffect(() => {
    if (token) {
      ensureDefault();
    }
  }, [token, ensureDefault]);

  // 同步深色模式到 <html class="dark">
  useEffect(() => {
    const apply = () => {
      const mql = window.matchMedia('(prefers-color-scheme: dark)');
      const effective =
        darkMode === 'system' ? mql.matches : darkMode === 'dark';
      document.documentElement.classList.toggle('dark', effective);
    };
    apply();
    if (darkMode === 'system') {
      const mql = window.matchMedia('(prefers-color-scheme: dark)');
      mql.addEventListener('change', apply);
      return () => mql.removeEventListener('change', apply);
    }
    return undefined;
  }, [darkMode]);

  if (!token) {
    return <LoginView />;
  }

  return (
    <div className="flex min-h-screen flex-col">
      <AppBar />
      <main className="flex-1 overflow-hidden">
        <StickyBoard />
      </main>
    </div>
  );
}
