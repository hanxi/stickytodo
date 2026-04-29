import { useState } from 'react';
import { LogOut, Moon, Plus, Sun, History as HistoryIcon, Heart } from 'lucide-react';
import { useMutation, useQueryClient } from '@tanstack/react-query';
import { useAuthStore } from '../store/authStore';
import { useUiStore } from '../store/uiStore';
import { api } from '../api/client';
import { queryKeys } from '../api/queryKeys';
import { DEFAULT_STICKY_COLOR, defaultFilter, type StickyView } from '../types/sticky';
import HistoryView from './HistoryView';
import SponsorModal from './SponsorModal';

/**
 * 生成新便签 id。优先 crypto.randomUUID（现代浏览器原生，格式符合后端
 * [A-Za-z0-9_-]+ 正则），降级到基于时间戳和随机数的字符串，保证永不冲突
 * 且满足 <=64 字符限制。
 */
function newStickyId(): string {
  if (typeof crypto !== 'undefined' && 'randomUUID' in crypto) {
    return crypto.randomUUID();
  }
  return `sticky-${Date.now().toString(36)}-${Math.random().toString(36).slice(2, 10)}`;
}

export default function AppBar() {
  const username = useAuthStore((s) => s.username);
  const logout = useAuthStore((s) => s.logout);
  const darkMode = useUiStore((s) => s.darkMode);
  const setDarkMode = useUiStore((s) => s.setDarkMode);
  const queryClient = useQueryClient();

  // 新建便签：client 端预生成 UUID，直接走 upsertSticky（PUT）让后端幂等落盘。
  //
  // 乐观更新：onMutate 里提前把"占位"便签插入列表让用户立刻看到新卡片；
  //   - onError: ctx.previous 整表回滚
  //   - onSuccess: 用服务端返回的权威视图替换占位条目（补齐 createdAt/updatedAt）
  //
  // mutationFn 在闭包里生成 id，并通过 variables 传给生命周期钩子，保证
  // onMutate 放入列表的占位 id 与 mutationFn 提交给服务端的 id 完全一致——
  // 若生成两个不同 id，会出现"占位便签永远留在列表、服务端那条永远补不回来"。
  const addStickyMutation = useMutation({
    mutationFn: (vars: { id: string }) =>
      api.upsertSticky({
        id: vars.id,
        title: '新便签',
        bgColor: DEFAULT_STICKY_COLOR,
        filter: { ...defaultFilter },
      }),
    onMutate: async (vars) => {
      await queryClient.cancelQueries({ queryKey: queryKeys.stickies() });
      const previous = queryClient.getQueryData<StickyView[]>(
        queryKeys.stickies(),
      );
      // 占位视图：服务端时间戳暂时用本机时间；onSuccess 会用服务端权威值覆盖
      const nowISO = new Date().toISOString();
      const placeholder: StickyView = {
        id: vars.id,
        title: '新便签',
        bgColor: DEFAULT_STICKY_COLOR,
        filter: { ...defaultFilter },
        createdAt: nowISO,
        updatedAt: nowISO,
      };
      queryClient.setQueryData<StickyView[]>(queryKeys.stickies(), (prev) => {
        if (!prev) return [placeholder];
        // 极小概率：同 id 已存在（重试场景）——不重复加
        if (prev.some((s) => s.id === placeholder.id)) return prev;
        return [...prev, placeholder];
      });
      return { previous };
    },
    onError: (_err, _vars, ctx) => {
      if (ctx?.previous) {
        queryClient.setQueryData(queryKeys.stickies(), ctx.previous);
      }
    },
    onSuccess: (created) => {
      // 用服务端权威视图替换占位条目（补齐 createdAt/updatedAt）。
      // 若跨端的 WS 事件已经先一步触发 list refetch 把本条加进来，这里的
      // findIndex 仍能命中并做一次幂等替换，不会产生重复。
      queryClient.setQueryData<StickyView[]>(queryKeys.stickies(), (prev) => {
        if (!prev) return [created];
        const idx = prev.findIndex((s) => s.id === created.id);
        if (idx === -1) return [...prev, created];
        const next = prev.slice();
        next[idx] = created;
        return next;
      });
    },
  });

  const [showHistory, setShowHistory] = useState(false);
  const [showSponsor, setShowSponsor] = useState(false);

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
            onClick={() => addStickyMutation.mutate({ id: newStickyId() })}
            disabled={addStickyMutation.isPending}
            className="inline-flex items-center gap-1 rounded px-2 py-1 text-sm hover:bg-gray-100 disabled:opacity-50 dark:hover:bg-neutral-700"
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
          <button
            type="button"
            onClick={() => setShowSponsor(true)}
            className="inline-flex items-center gap-1 rounded px-2 py-1 text-sm text-pink-600 hover:bg-pink-50 dark:text-pink-400 dark:hover:bg-pink-900/30"
            title="赞赏支持"
          >
            <Heart size={16} /> 赞赏
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
      <SponsorModal open={showSponsor} onClose={() => setShowSponsor(false)} />
    </>
  );
}
