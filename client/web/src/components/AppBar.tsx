import { useState } from 'react';
import { LogOut, Moon, Plus, Sun, History as HistoryIcon } from 'lucide-react';
import { useAuthStore } from '../store/authStore';
import { useStickyStore } from '../store/stickyStore';
import { useUiStore } from '../store/uiStore';
import HistoryView from './HistoryView';

export default function AppBar() {
  const username = useAuthStore((s) => s.username);
  const logout = useAuthStore((s) => s.logout);
  const addSticky = useStickyStore((s) => s.addSticky);
  const darkMode = useUiStore((s) => s.darkMode);
  const setDarkMode = useUiStore((s) => s.setDarkMode);

  const [showHistory, setShowHistory] = useState(false);

  // 系统/手动两段切换；点击按钮时：system → dark → light → system
  const nextDark =
    darkMode === 'system' ? 'dark' : darkMode === 'dark' ? 'light' : 'system';
  const darkLabel =
    darkMode === 'system'
      ? '跟随系统'
      : darkMode === 'dark'
        ? '深色'
        : '浅色';

  return (
    <>
      <header className="flex items-center justify-between border-b border-gray-200 bg-white px-4 py-2 dark:border-neutral-700 dark:bg-neutral-800">
        <div className="flex items-center gap-2">
          <span className="text-base font-semibold">StickyTodo</span>
          <span className="text-xs text-gray-400">Web</span>
        </div>
        <div className="flex items-center gap-1">
          <button
            type="button"
            onClick={() => addSticky()}
            className="inline-flex items-center gap-1 rounded px-2 py-1 text-sm hover:bg-gray-100 dark:hover:bg-neutral-700"
            title="新建便签"
          >
            <Plus size={16} /> 新建便签
          </button>
          <button
            type="button"
            onClick={() => setShowHistory(true)}
            className="inline-flex items-center gap-1 rounded px-2 py-1 text-sm hover:bg-gray-100 dark:hover:bg-neutral-700"
            title="全局审计"
          >
            <HistoryIcon size={16} /> 全局历史
          </button>
          <button
            type="button"
            onClick={() => setDarkMode(nextDark)}
            className="inline-flex items-center gap-1 rounded px-2 py-1 text-sm hover:bg-gray-100 dark:hover:bg-neutral-700"
            title={`深色模式：${darkLabel}（点击切换）`}
          >
            {darkMode === 'dark' ? <Moon size={16} /> : <Sun size={16} />}
            <span className="hidden sm:inline">{darkLabel}</span>
          </button>
          <div className="mx-2 h-5 w-px bg-gray-200 dark:bg-neutral-700" />
          <span className="hidden px-1 text-xs text-gray-500 sm:inline dark:text-gray-400">
            {username}
          </span>
          <button
            type="button"
            onClick={logout}
            className="inline-flex items-center gap-1 rounded px-2 py-1 text-sm text-red-600 hover:bg-red-50 dark:text-red-400 dark:hover:bg-red-900/30"
            title="登出"
          >
            <LogOut size={16} /> 登出
          </button>
        </div>
      </header>
      {showHistory ? (
        <HistoryView
          mode={{ kind: 'global' }}
          onClose={() => setShowHistory(false)}
        />
      ) : null}
    </>
  );
}
