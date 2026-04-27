import { useQuery } from '@tanstack/react-query';
import { api } from '../api/client';
import { queryKeys } from '../api/queryKeys';
import type { TodoFilter } from '../types/sticky';
import TodoRow from './TodoRow';
import DraftTodoRow from './DraftTodoRow';
import { useStickyStore } from '../store/stickyStore';

interface Props {
  noteId: string;
  filter: TodoFilter;
  bgColor: string;
}

export default function TodoList({ noteId, filter, bgColor }: Props) {
  const updateFilter = useStickyStore((s) => s.updateFilter);

  const { data, isLoading, isError, error } = useQuery({
    queryKey: queryKeys.todos(filter),
    queryFn: () => api.listTodos(filter),
  });

  return (
    <div className="space-y-1">
      <DraftTodoRow filter={filter} />

      {isLoading ? (
        <div className="py-4 text-center text-xs opacity-60">加载中…</div>
      ) : isError ? (
        <div className="py-4 text-center text-xs text-red-600 dark:text-red-400">
          加载失败：{(error as Error).message}
        </div>
      ) : !data || data.items.length === 0 ? (
        <div className="py-4 text-center text-xs opacity-60">
          空空如也，在顶部输入标题按回车创建
        </div>
      ) : (
        data.items.map((t) => (
          <TodoRow key={t.id} todo={t} bgColor={bgColor} />
        ))
      )}

      {data && data.total > data.items.length ? (
        <div className="pt-2 text-center">
          <button
            type="button"
            className="rounded border border-black/10 px-2 py-1 text-xs hover:bg-black/5 dark:border-white/20 dark:hover:bg-white/5"
            onClick={() =>
              updateFilter(noteId, { page_size: filter.page_size + 50 })
            }
          >
            加载更多（{data.items.length}/{data.total}）
          </button>
        </div>
      ) : null}
    </div>
  );
}
