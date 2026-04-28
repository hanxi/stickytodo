import { useState } from 'react';
import { MoreHorizontal, X, Filter as FilterIcon } from 'lucide-react';
import { useMutation, useQueryClient } from '@tanstack/react-query';
import { api } from '../api/client';
import { queryKeys } from '../api/queryKeys';
import {
  STICKY_COLORS,
  defaultFilter,
  type StickyView,
  type TodoFilter,
} from '../types/sticky';
import { foregroundFor, isLightBackground } from '../lib/color';
import { filterSummary } from '../lib/format';
import TodoList from './TodoList';
import FilterEditor from './FilterEditor';

interface Props {
  sticky: StickyView;
}

/**
 * 单张便签卡片。
 *
 * 数据源：父层 StickyBoard 从 useQuery(['stickies']) 拿到 StickyView[] 后把单条对象
 * 传进来。本组件不再读 store，写操作一律通过 mutations 走 api.upsertSticky /
 * api.deleteSticky。
 *
 * 乐观更新 + 权威回填策略：
 *   - onMutate:  立刻用乐观值替换列表里本条 sticky（用户毫秒级见到反馈）
 *   - onError:   用 ctx.previous 快照整表回滚
 *   - onSuccess: 用服务端返回的 StickyView（含权威 updatedAt）就地替换本条，
 *                使 createdAt/updatedAt 等服务端字段立刻可见，避免停留在旧值
 *
 * 为什么 onSuccess **必须**自己写 cache，不能"等 WS 兜底"：
 *   后端 hub.Broadcast 目前会向所有客户端（包括发起端）广播事件。如果本端
 *   只靠 WS 触发 invalidate，会出现：
 *     a) 先拿不到服务端返回的 updatedAt（要等下一次 list 才能补齐），
 *     b) mutation 未完成时 WS 已到达，引发与 mutationFn Promise 的竞速。
 *   本端写操作的最佳源就是 mutation 自己的响应体；WS 的价值在于跨端场景
 *   （其他客户端写入，本端用 invalidate 跟进）。
 */
export default function StickyCard({ sticky }: Props) {
  const queryClient = useQueryClient();

  const [titleEditing, setTitleEditing] = useState(false);
  const [draftTitle, setDraftTitle] = useState('');
  const [menuOpen, setMenuOpen] = useState(false);
  const [filterOpen, setFilterOpen] = useState(false);

  const fg = foregroundFor(sticky.bgColor);
  const borderClass = isLightBackground(sticky.bgColor)
    ? 'border-black/10'
    : 'border-white/20';

  // upsertMutation：入参为"StickyView 的可写子集"，服务端响应为权威结果。
  const upsertMutation = useMutation({
    mutationFn: (patch: { title: string; bgColor: string; filter: TodoFilter }) =>
      api.upsertSticky({
        id: sticky.id,
        title: patch.title,
        bgColor: patch.bgColor,
        filter: patch.filter,
      }),
    onMutate: async (patch) => {
      // 乐观更新：先把列表里本条 sticky 替换掉
      await queryClient.cancelQueries({ queryKey: queryKeys.stickies() });
      const previous = queryClient.getQueryData<StickyView[]>(
        queryKeys.stickies(),
      );
      if (previous) {
        queryClient.setQueryData<StickyView[]>(
          queryKeys.stickies(),
          previous.map((s) =>
            s.id === sticky.id
              ? {
                  ...s,
                  title: patch.title,
                  bgColor: patch.bgColor,
                  filter: patch.filter,
                }
              : s,
          ),
        );
      }
      return { previous };
    },
    onError: (_err, _patch, ctx) => {
      if (ctx?.previous) {
        queryClient.setQueryData(queryKeys.stickies(), ctx.previous);
      }
    },
    onSuccess: (serverView) => {
      // 用服务端权威结果替换乐观值。如果列表里已经不存在这条（极端情况下：
      // 同用户的其他端先一步删除并通过 WS 广播触发了本端列表更新），就原样不动，
      // 避免把已被删除的便签"复活"。
      queryClient.setQueryData<StickyView[]>(queryKeys.stickies(), (prev) => {
        if (!prev) return [serverView];
        const idx = prev.findIndex((s) => s.id === serverView.id);
        if (idx === -1) return prev;
        const next = prev.slice();
        next[idx] = serverView;
        return next;
      });
    },
  });

  const deleteMutation = useMutation({
    mutationFn: () => api.deleteSticky(sticky.id),
    onMutate: async () => {
      await queryClient.cancelQueries({ queryKey: queryKeys.stickies() });
      const previous = queryClient.getQueryData<StickyView[]>(
        queryKeys.stickies(),
      );
      if (previous) {
        queryClient.setQueryData<StickyView[]>(
          queryKeys.stickies(),
          previous.filter((s) => s.id !== sticky.id),
        );
      }
      return { previous };
    },
    onError: (_err, _vars, ctx) => {
      if (ctx?.previous) {
        queryClient.setQueryData(queryKeys.stickies(), ctx.previous);
      }
    },
    // onSuccess 无需额外写 cache：onMutate 的乐观删除已经把本条移出列表，
    // 服务端返回的 { id, deleted: true } 没有更多可回填的视图信息。
  });

  function commitTitle() {
    const t = draftTitle.trim();
    if (t && t !== sticky.title) {
      upsertMutation.mutate({
        title: t,
        bgColor: sticky.bgColor,
        filter: sticky.filter,
      });
    }
    setTitleEditing(false);
  }

  function changeColor(hex: string) {
    upsertMutation.mutate({
      title: sticky.title,
      bgColor: hex,
      filter: sticky.filter,
    });
  }

  function resetFilter() {
    upsertMutation.mutate({
      title: sticky.title,
      bgColor: sticky.bgColor,
      filter: { ...defaultFilter },
    });
  }

  function applyFilter(next: TodoFilter) {
    upsertMutation.mutate({
      title: sticky.title,
      bgColor: sticky.bgColor,
      filter: next,
    });
  }

  return (
    <div
      className={`flex max-h-[80vh] flex-col rounded-lg border shadow-sm ${borderClass}`}
      style={{ backgroundColor: sticky.bgColor, color: fg }}
    >
      {/* 标题栏 */}
      <div className="flex items-center justify-between gap-2 border-b border-black/5 px-3 py-2 dark:border-white/10">
        {titleEditing ? (
          <input
            autoFocus
            className="flex-1 bg-transparent text-sm font-semibold outline-none"
            value={draftTitle}
            onChange={(e) => setDraftTitle(e.target.value)}
            onBlur={commitTitle}
            onKeyDown={(e) => {
              if (e.key === 'Enter') commitTitle();
              if (e.key === 'Escape') setTitleEditing(false);
            }}
            style={{ color: fg }}
          />
        ) : (
          <button
            type="button"
            className="flex-1 truncate text-left text-sm font-semibold"
            onClick={() => {
              setDraftTitle(sticky.title);
              setTitleEditing(true);
            }}
            title="点击编辑标题"
          >
            {sticky.title || '未命名便签'}
          </button>
        )}

        {/* 菜单按钮 */}
        <div className="relative">
          <button
            type="button"
            className="rounded p-1 opacity-70 hover:opacity-100"
            onClick={() => setMenuOpen((v) => !v)}
            title="更多"
          >
            <MoreHorizontal size={16} />
          </button>
          {menuOpen ? (
            <div
              className="absolute right-0 top-full z-20 mt-1 min-w-[160px] rounded border border-gray-200 bg-white p-1 text-sm text-gray-900 shadow-lg dark:border-neutral-600 dark:bg-neutral-800 dark:text-gray-100"
              onMouseLeave={() => setMenuOpen(false)}
            >
              <div className="px-2 py-1 text-xs text-gray-500">换色</div>
              <div className="flex gap-1 px-2 pb-1">
                {STICKY_COLORS.map((c) => (
                  <button
                    key={c.hex}
                    type="button"
                    title={c.name}
                    onClick={() => {
                      changeColor(c.hex);
                      setMenuOpen(false);
                    }}
                    className="h-5 w-5 rounded-full border border-black/10"
                    style={{ backgroundColor: c.hex }}
                  />
                ))}
              </div>
              <hr className="my-1 border-gray-200 dark:border-neutral-600" />
              <button
                type="button"
                className="block w-full rounded px-2 py-1 text-left hover:bg-gray-100 dark:hover:bg-neutral-700"
                onClick={() => {
                  resetFilter();
                  setMenuOpen(false);
                }}
              >
                重置筛选
              </button>
            </div>
          ) : null}
        </div>

        <button
          type="button"
          className="rounded p-1 opacity-70 hover:opacity-100"
          title="删除便签"
          onClick={() => {
            if (
              confirm(
                `确定删除便签「${sticky.title || '未命名'}」？会同步到所有登录此账号的设备。`,
              )
            ) {
              deleteMutation.mutate();
            }
          }}
        >
          <X size={16} />
        </button>
      </div>

      {/* 内容：Todo 列表 */}
      <div className="flex-1 overflow-auto thin-scroll px-3 py-2">
        <TodoList
          filter={sticky.filter}
          bgColor={sticky.bgColor}
          onFilterChange={applyFilter}
        />
      </div>

      {/* 底部筛选条 */}
      <button
        type="button"
        onClick={() => setFilterOpen(true)}
        className="flex items-center justify-between gap-2 border-t border-black/5 px-3 py-2 text-xs hover:bg-black/5 dark:border-white/10 dark:hover:bg-white/5"
        title="编辑筛选"
      >
        <span className="flex items-center gap-1 truncate">
          <FilterIcon size={12} />
          <span className="truncate">{filterSummary(sticky.filter)}</span>
        </span>
        <span className="opacity-70">编辑</span>
      </button>

      {filterOpen ? (
        <FilterEditor
          initialFilter={sticky.filter}
          onClose={() => setFilterOpen(false)}
          onSave={(f) => {
            applyFilter(f);
            setFilterOpen(false);
          }}
        />
      ) : null}
    </div>
  );
}
