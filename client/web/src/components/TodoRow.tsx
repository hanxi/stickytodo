import { useState } from 'react';
import { Flag, MoreHorizontal, Pencil, RotateCcw, Trash2, History as HistoryIcon } from 'lucide-react';
import { useMutation, useQueryClient } from '@tanstack/react-query';
import { api } from '../api/client';
import type { Todo } from '../types/api';
import { foregroundFor, priorityColor } from '../lib/color';
import { formatDue } from '../lib/format';
import EditTodoSheet from './EditTodoSheet';
import HistoryView from './HistoryView';

interface Props {
  todo: Todo;
  bgColor: string;
}

export default function TodoRow({ todo, bgColor }: Props) {
  const qc = useQueryClient();
  const fg = foregroundFor(bgColor);
  const flagColor = priorityColor(todo.priority, bgColor);

  const [editing, setEditing] = useState(false);
  const [titleEditing, setTitleEditing] = useState(false);
  const [tagEditing, setTagEditing] = useState(false);
  const [draftTitle, setDraftTitle] = useState(todo.title);
  const [draftTag, setDraftTag] = useState(todo.tag);
  const [menuOpen, setMenuOpen] = useState(false);
  const [historyOpen, setHistoryOpen] = useState(false);

  const invalidate = () => {
    qc.invalidateQueries({ queryKey: ['todos'] });
    qc.invalidateQueries({ queryKey: ['tags'] });
  };

  const complete = useMutation({ mutationFn: () => api.completeTodo(todo.id), onSuccess: invalidate });
  const reopen = useMutation({ mutationFn: () => api.reopenTodo(todo.id), onSuccess: invalidate });
  const remove = useMutation({ mutationFn: () => api.deleteTodo(todo.id), onSuccess: invalidate });
  const restore = useMutation({ mutationFn: () => api.restoreTodo(todo.id), onSuccess: invalidate });
  const patch = useMutation({
    mutationFn: (body: Parameters<typeof api.updateTodo>[1]) =>
      api.updateTodo(todo.id, body),
    onSuccess: invalidate,
  });

  const isDeleted = todo.deleted_at !== null;
  const done = todo.status === 'done';

  function commitTitle() {
    const t = draftTitle.trim();
    setTitleEditing(false);
    if (t && t !== todo.title) {
      patch.mutate({ title: t });
    } else {
      setDraftTitle(todo.title);
    }
  }
  function commitTag() {
    setTagEditing(false);
    if (draftTag !== todo.tag) {
      patch.mutate({ tag: draftTag });
    }
  }

  const metaParts: string[] = [];
  if (todo.tag) metaParts.push(`#${todo.tag}`);
  if (todo.due_at) metaParts.push(`📅 ${formatDue(todo.due_at)}`);
  const hasMeta = metaParts.length > 0 || tagEditing;

  return (
    <>
      <div
        className={`group flex items-start gap-2 rounded px-1.5 py-1 hover:bg-black/5 dark:hover:bg-white/5 ${
          isDeleted ? 'opacity-60' : ''
        }`}
      >
        {/* checkbox */}
        <button
          type="button"
          disabled={isDeleted}
          title={done ? '标记为未完成' : '标记为完成'}
          onClick={() => (done ? reopen.mutate() : complete.mutate())}
          className={`mt-0.5 h-4 w-4 shrink-0 rounded border ${
            done
              ? 'border-transparent bg-emerald-500'
              : 'border-current bg-transparent'
          }`}
        >
          {done ? (
            <svg
              viewBox="0 0 16 16"
              className="h-full w-full text-white"
              fill="none"
              stroke="currentColor"
              strokeWidth="2"
            >
              <path d="M3 8l3 3 7-7" />
            </svg>
          ) : null}
        </button>

        {/* 优先级旗 */}
        {flagColor ? (
          <Flag
            size={14}
            className="mt-1 shrink-0"
            style={{ color: flagColor, fill: flagColor }}
          />
        ) : null}

        {/* 主体 */}
        <div className="min-w-0 flex-1">
          {titleEditing ? (
            <input
              autoFocus
              className="w-full bg-transparent text-sm outline-none"
              value={draftTitle}
              onChange={(e) => setDraftTitle(e.target.value)}
              onBlur={commitTitle}
              onKeyDown={(e) => {
                if (e.key === 'Enter') commitTitle();
                if (e.key === 'Escape') {
                  setDraftTitle(todo.title);
                  setTitleEditing(false);
                }
              }}
              style={{ color: fg }}
            />
          ) : (
            <button
              type="button"
              className={`block w-full truncate text-left text-sm ${done ? 'line-through opacity-60' : ''}`}
              onClick={() => setTitleEditing(true)}
              title={todo.title}
            >
              {todo.title}
            </button>
          )}

          {hasMeta ? (
            <div className="mt-0.5 flex items-center gap-2 text-[11px] opacity-70">
              {tagEditing ? (
                <input
                  autoFocus
                  className="w-20 bg-transparent outline-none"
                  value={draftTag}
                  onChange={(e) => setDraftTag(e.target.value)}
                  onBlur={commitTag}
                  onKeyDown={(e) => {
                    if (e.key === 'Enter') commitTag();
                    if (e.key === 'Escape') {
                      setDraftTag(todo.tag);
                      setTagEditing(false);
                    }
                  }}
                  style={{ color: fg }}
                />
              ) : (
                <button
                  type="button"
                  onClick={() => setTagEditing(true)}
                  title="点击编辑标签"
                >
                  {todo.tag ? `#${todo.tag}` : '+标签'}
                </button>
              )}
              {todo.due_at ? <span>📅 {formatDue(todo.due_at)}</span> : null}
            </div>
          ) : null}
        </div>

        {/* 菜单 */}
        <div className="relative opacity-0 transition group-hover:opacity-100">
          <button
            type="button"
            className="rounded p-1"
            onClick={() => setMenuOpen((v) => !v)}
            title="更多"
          >
            <MoreHorizontal size={14} />
          </button>
          {menuOpen ? (
            <div
              className="absolute right-0 top-full z-20 mt-1 min-w-[140px] rounded border border-gray-200 bg-white p-1 text-xs text-gray-900 shadow-lg dark:border-neutral-600 dark:bg-neutral-800 dark:text-gray-100"
              onMouseLeave={() => setMenuOpen(false)}
            >
              <button
                type="button"
                className="flex w-full items-center gap-2 rounded px-2 py-1 text-left hover:bg-gray-100 dark:hover:bg-neutral-700"
                onClick={() => {
                  setMenuOpen(false);
                  setEditing(true);
                }}
              >
                <Pencil size={12} /> 编辑详情
              </button>
              <button
                type="button"
                className="flex w-full items-center gap-2 rounded px-2 py-1 text-left hover:bg-gray-100 dark:hover:bg-neutral-700"
                onClick={() => {
                  setMenuOpen(false);
                  setHistoryOpen(true);
                }}
              >
                <HistoryIcon size={12} /> 历史
              </button>
              <hr className="my-1 border-gray-200 dark:border-neutral-600" />
              {isDeleted ? (
                <button
                  type="button"
                  className="flex w-full items-center gap-2 rounded px-2 py-1 text-left text-emerald-700 hover:bg-emerald-50 dark:text-emerald-300 dark:hover:bg-emerald-900/30"
                  onClick={() => {
                    setMenuOpen(false);
                    restore.mutate();
                  }}
                >
                  <RotateCcw size={12} /> 恢复
                </button>
              ) : (
                <button
                  type="button"
                  className="flex w-full items-center gap-2 rounded px-2 py-1 text-left text-red-700 hover:bg-red-50 dark:text-red-400 dark:hover:bg-red-900/30"
                  onClick={() => {
                    setMenuOpen(false);
                    remove.mutate();
                  }}
                >
                  <Trash2 size={12} /> 删除
                </button>
              )}
            </div>
          ) : null}
        </div>
      </div>

      {editing ? (
        <EditTodoSheet todo={todo} onClose={() => setEditing(false)} />
      ) : null}

      {historyOpen ? (
        <HistoryView
          mode={{ kind: 'todo', todoId: todo.id, title: todo.title }}
          onClose={() => setHistoryOpen(false)}
        />
      ) : null}
    </>
  );
}
