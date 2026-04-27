import { useState, type KeyboardEvent } from 'react';
import { useMutation, useQueryClient } from '@tanstack/react-query';
import { api } from '../api/client';
import type { TodoFilter } from '../types/sticky';

interface Props {
  filter: TodoFilter;
}

export default function DraftTodoRow({ filter }: Props) {
  const [title, setTitle] = useState('');
  const qc = useQueryClient();

  const createMutation = useMutation({
    mutationFn: api.createTodo,
    onSuccess: () => {
      qc.invalidateQueries({ queryKey: ['todos'] });
      qc.invalidateQueries({ queryKey: ['tags'] });
      setTitle('');
    },
  });

  function onKey(e: KeyboardEvent<HTMLInputElement>) {
    if (e.key === 'Enter') {
      e.preventDefault();
      const t = title.trim();
      if (!t) return;
      // 若当前便签筛选了标签，就默认把新 todo 也打上这个标签，行为与 macOS 客户端一致
      createMutation.mutate({
        title: t,
        tag: filter.tag || undefined,
      });
    } else if (e.key === 'Escape') {
      setTitle('');
    }
  }

  return (
    <div className="flex items-center gap-2 rounded border border-dashed border-black/20 bg-black/5 px-2 py-1.5 dark:border-white/20 dark:bg-white/5">
      <span className="opacity-60">＋</span>
      <input
        className="flex-1 bg-transparent text-sm outline-none placeholder:opacity-50"
        placeholder="新建待办，回车创建；Esc 清空"
        value={title}
        onChange={(e) => setTitle(e.target.value)}
        onKeyDown={onKey}
        disabled={createMutation.isPending}
      />
      {createMutation.isPending ? (
        <span className="text-xs opacity-60">创建中…</span>
      ) : null}
    </div>
  );
}
