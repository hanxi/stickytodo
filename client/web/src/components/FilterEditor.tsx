import { useState } from 'react';
import Modal from './Modal';
import type { TodoFilter } from '../types/sticky';
import { defaultFilter } from '../types/sticky';
import { toISOFromLocalInput, toLocalInputFromISO } from '../lib/format';

interface Props {
  /** 打开时的初始筛选条件（通常来自所属便签的 sticky.filter）。 */
  initialFilter: TodoFilter;
  /** 取消 / 关闭时调用，不保存。 */
  onClose: () => void;
  /**
   * 保存时调用，参数是最终生效的 filter（已自动把 page 重置为 1）。
   * 调用方负责把这个 filter 写回服务端（通过 upsertSticky），本组件不关心持久化介质。
   */
  onSave: (filter: TodoFilter) => void;
}

/**
 * 便签筛选条件编辑器（Modal）。
 *
 * 与旧实现的区别：不再直接读写 stickyStore。所有写入由父组件通过 onSave 回调
 * 委托给 upsertSticky mutation，保持"一个便签的所有写操作都经 mutation 走云端"
 * 这条统一路径。
 */
export default function FilterEditor({ initialFilter, onClose, onSave }: Props) {
  const [draft, setDraft] = useState<TodoFilter>(() => ({ ...initialFilter }));

  function update<K extends keyof TodoFilter>(key: K, value: TodoFilter[K]) {
    setDraft((d) => ({ ...d, [key]: value }));
  }

  function save() {
    // 任何筛选条件变更都把分页复位到第 1 页，避免 page 数超过新结果总页数导致空列表
    onSave({ ...draft, page: 1 });
  }

  return (
    <Modal
      open
      onClose={onClose}
      title="编辑筛选"
      footer={
        <>
          <button
            type="button"
            onClick={() => setDraft({ ...defaultFilter })}
            className="rounded px-3 py-1.5 text-sm hover:bg-gray-100 dark:hover:bg-neutral-700"
          >
            重置
          </button>
          <button
            type="button"
            onClick={onClose}
            className="rounded px-3 py-1.5 text-sm hover:bg-gray-100 dark:hover:bg-neutral-700"
          >
            取消
          </button>
          <button
            type="button"
            onClick={save}
            className="rounded bg-blue-600 px-3 py-1.5 text-sm text-white hover:bg-blue-700"
          >
            保存
          </button>
        </>
      }
    >
      <div className="grid grid-cols-1 gap-3 text-sm">
        <label>
          <span className="mb-1 block text-xs text-gray-500">状态</span>
          <select
            className="w-full rounded border border-gray-300 bg-white px-2 py-1 dark:border-neutral-600 dark:bg-neutral-700"
            value={draft.status ?? 'all'}
            onChange={(e) =>
              update('status', e.target.value as TodoFilter['status'])
            }
          >
            <option value="all">全部</option>
            <option value="pending">未完成</option>
            <option value="done">已完成</option>
          </select>
        </label>

        <label>
          <span className="mb-1 block text-xs text-gray-500">标签</span>
          <input
            type="text"
            className="w-full rounded border border-gray-300 bg-white px-2 py-1 dark:border-neutral-600 dark:bg-neutral-700"
            value={draft.tag}
            onChange={(e) => update('tag', e.target.value)}
            placeholder="不填 = 不筛选"
          />
        </label>

        <label>
          <span className="mb-1 block text-xs text-gray-500">关键词</span>
          <input
            type="text"
            className="w-full rounded border border-gray-300 bg-white px-2 py-1 dark:border-neutral-600 dark:bg-neutral-700"
            value={draft.keyword}
            onChange={(e) => update('keyword', e.target.value)}
            placeholder="标题/正文搜索"
          />
        </label>

        <label>
          <span className="mb-1 block text-xs text-gray-500">截止时间之前</span>
          <input
            type="datetime-local"
            className="w-full rounded border border-gray-300 bg-white px-2 py-1 dark:border-neutral-600 dark:bg-neutral-700"
            value={toLocalInputFromISO(draft.due_before)}
            onChange={(e) =>
              update('due_before', toISOFromLocalInput(e.target.value))
            }
          />
        </label>

        <div className="flex items-center gap-4">
          <label className="inline-flex items-center gap-2">
            <input
              type="checkbox"
              checked={draft.include_deleted}
              onChange={(e) => update('include_deleted', e.target.checked)}
            />
            <span>含已删除</span>
          </label>
          <label className="inline-flex items-center gap-2">
            <input
              type="checkbox"
              checked={draft.only_deleted}
              onChange={(e) => update('only_deleted', e.target.checked)}
            />
            <span>仅看已删除</span>
          </label>
        </div>

        <label>
          <span className="mb-1 block text-xs text-gray-500">每页条数</span>
          <input
            type="number"
            min={10}
            max={500}
            step={10}
            className="w-full rounded border border-gray-300 bg-white px-2 py-1 dark:border-neutral-600 dark:bg-neutral-700"
            value={draft.page_size}
            onChange={(e) =>
              update('page_size', Math.max(10, Number(e.target.value) || 50))
            }
          />
        </label>
      </div>
    </Modal>
  );
}
