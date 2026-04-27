import { useState } from 'react';
import { MoreHorizontal, X, Filter as FilterIcon } from 'lucide-react';
import { useStickyStore } from '../store/stickyStore';
import { STICKY_COLORS, defaultFilter } from '../types/sticky';
import { foregroundFor, isLightBackground } from '../lib/color';
import { filterSummary } from '../lib/format';
import TodoList from './TodoList';
import FilterEditor from './FilterEditor';

interface Props {
  noteId: string;
}

export default function StickyCard({ noteId }: Props) {
  const note = useStickyStore((s) => s.stickies.find((n) => n.id === noteId));
  const updateSticky = useStickyStore((s) => s.updateSticky);
  const removeSticky = useStickyStore((s) => s.removeSticky);
  const replaceFilter = useStickyStore((s) => s.replaceFilter);

  const [titleEditing, setTitleEditing] = useState(false);
  const [draftTitle, setDraftTitle] = useState('');
  const [menuOpen, setMenuOpen] = useState(false);
  const [filterOpen, setFilterOpen] = useState(false);

  if (!note) return null;

  const fg = foregroundFor(note.color);
  const borderClass = isLightBackground(note.color)
    ? 'border-black/10'
    : 'border-white/20';

  function commitTitle() {
    if (!note) return;
    const t = draftTitle.trim();
    if (t && t !== note.title) {
      updateSticky(note.id, { title: t });
    }
    setTitleEditing(false);
  }

  return (
    <div
      className={`flex max-h-[80vh] flex-col rounded-lg border shadow-sm ${borderClass}`}
      style={{ backgroundColor: note.color, color: fg }}
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
              setDraftTitle(note.title);
              setTitleEditing(true);
            }}
            title="点击编辑标题"
          >
            {note.title || '未命名便签'}
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
                      updateSticky(note.id, { color: c.hex });
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
                  replaceFilter(note.id, { ...defaultFilter });
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
            if (confirm(`确定删除便签「${note.title}」？本地操作，不影响 todo。`)) {
              removeSticky(note.id);
            }
          }}
        >
          <X size={16} />
        </button>
      </div>

      {/* 内容：Todo 列表 */}
      <div className="flex-1 overflow-auto thin-scroll px-3 py-2">
        <TodoList noteId={note.id} filter={note.filter} bgColor={note.color} />
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
          <span className="truncate">{filterSummary(note.filter)}</span>
        </span>
        <span className="opacity-70">编辑</span>
      </button>

      {filterOpen ? (
        <FilterEditor
          noteId={note.id}
          onClose={() => setFilterOpen(false)}
        />
      ) : null}
    </div>
  );
}
