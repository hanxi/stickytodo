import { useEffect } from 'react';
import { useAuthStore } from './store/authStore';
import { useUiStore } from './store/uiStore';
import { useRealtimeSync } from './hooks/useRealtimeSync';
import LoginView from './views/LoginView';
import StickyBoard from './views/StickyBoard';
import AppBar from './components/AppBar';

export default function App() {
  const token = useAuthStore((s) => s.token);
  const hydrateCheckExpiry = useAuthStore((s) => s.hydrateCheckExpiry);
  const darkMode = useUiStore((s) => s.darkMode);

  // 启动时检查 token 过期
  useEffect(() => {
    hydrateCheckExpiry();
  }, [hydrateCheckExpiry]);

  // 挂载 WebSocket 实时同步：内部按 authStore.token 自动 connect/disconnect
  useRealtimeSync();

  // 注："登录后确保至少一张便签"的语义由 StickyBoard 的空状态页承担（提示用户新建），
  // 不再在 App 层主动写一条默认便签——云端源的便签应完全由用户显式创建。

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
