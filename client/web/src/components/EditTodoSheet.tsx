import { useState } from 'react';
import { useMutation, useQueryClient } from '@tanstack/react-query';
import Modal from './Modal';
import { api } from '../api/client';
import type { Todo, UpdateTodoRequest } from '../types/api';
import { toISOFromLocalInput, toLocalInputFromISO } from '../lib/format';

interface Props {
  todo: Todo;
  onClose: () => void;
}

export default function EditTodoSheet({ todo, onClose }: Props) {
  const qc = useQueryClient();
  const [title, setTitle] = useState(todo.title);
  const [content, setContent] = useState(todo.content);
  const [tag, setTag] = useState(todo.tag);
  const [priority, setPriority] = useState(todo.priority);
  const [dueLocal, setDueLocal] = useState(toLocalInputFromISO(todo.due_at));

  const save = useMutation({
    mutationFn: (body: UpdateTodoRequest) => api.updateTodo(todo.id, body),
    onSuccess: () => {
      qc.invalidateQueries({ queryKey: ['todos'] });
      qc.invalidateQueries({ queryKey: ['tags'] });
      onClose();
    },
  });

  function onSave() {
    // 仅提交真正有变化的字段，和 macOS 客户端的 diff 行为一致
    const body: UpdateTodoRequest = {};
    if (title !== todo.title) body.title = title;
    if (content !== todo.content) body.content = content;
    if (tag !== todo.tag) body.tag = tag;
    if (priority !== todo.priority) body.priority = priority;

    const newDueISO = toISOFromLocalInput(dueLocal);
    const oldDueISO = todo.due_at;
    if (!dueLocal && oldDueISO) {
      body.clear_due_at = true;
    } else if (newDueISO && newDueISO !== oldDueISO) {
      body.due_at = newDueISO;
    }

    if (Object.keys(body).length === 0) {
      onClose();
      return;
    }
    save.mutate(body);
  }

  return (
    <Modal
      open
      onClose={onClose}
      title="编辑 Todo"
      maxWidth="max-w-xl"
      footer={
        <>
          <button
            type="button"
            className="rounded px-3 py-1.5 text-sm hover:bg-gray-100 dark:hover:bg-neutral-700"
            onClick={onClose}
          >
            取消
          </button>
          <button
            type="button"
            disabled={save.isPending}
            onClick={onSave}
            className="rounded bg-blue-600 px-3 py-1.5 text-sm text-white hover:bg-blue-700 disabled:opacity-60"
          >
            {save.isPending ? '保存中…' : '保存'}
          </button>
        </>
      }
    >
      <div className="space-y-3 text-sm">
        <label className="block">
          <span className="mb-1 block text-xs text-gray-500">标题</span>
          <input
            type="text"
            className="w-full rounded border border-gray-300 bg-white px-2 py-1 dark:border-neutral-600 dark:bg-neutral-700"
            value={title}
            onChange={(e) => setTitle(e.target.value)}
          />
        </label>
        <label className="block">
          <span className="mb-1 block text-xs text-gray-500">内容</span>
          <textarea
            rows={4}
            className="w-full rounded border border-gray-300 bg-white px-2 py-1 dark:border-neutral-600 dark:bg-neutral-700"
            value={content}
            onChange={(e) => setContent(e.target.value)}
          />
        </label>
        <div className="grid grid-cols-2 gap-3">
          <label className="block">
            <span className="mb-1 block text-xs text-gray-500">标签</span>
            <input
              type="text"
              className="w-full rounded border border-gray-300 bg-white px-2 py-1 dark:border-neutral-600 dark:bg-neutral-700"
              value={tag}
              onChange={(e) => setTag(e.target.value)}
            />
          </label>
          <label className="block">
            <span className="mb-1 block text-xs text-gray-500">优先级 0-3</span>
            <select
              className="w-full rounded border border-gray-300 bg-white px-2 py-1 dark:border-neutral-600 dark:bg-neutral-700"
              value={priority}
              onChange={(e) => setPriority(Number(e.target.value))}
            >
              <option value={0}>0 - 普通</option>
              <option value={1}>1 - 低</option>
              <option value={2}>2 - 中</option>
              <option value={3}>3 - 高</option>
            </select>
          </label>
        </div>
        <label className="block">
          <span className="mb-1 block text-xs text-gray-500">截止时间</span>
          <div className="flex items-center gap-2">
            <input
              type="datetime-local"
              className="flex-1 rounded border border-gray-300 bg-white px-2 py-1 dark:border-neutral-600 dark:bg-neutral-700"
              value={dueLocal}
              onChange={(e) => setDueLocal(e.target.value)}
            />
            {dueLocal ? (
              <button
                type="button"
                className="rounded border border-gray-300 px-2 py-1 text-xs hover:bg-gray-100 dark:border-neutral-600 dark:hover:bg-neutral-700"
                onClick={() => setDueLocal('')}
              >
                清空
              </button>
            ) : null}
          </div>
        </label>
        {save.isError ? (
          <div className="rounded bg-red-50 p-2 text-xs text-red-600 dark:bg-red-900/30 dark:text-red-300">
            {(save.error as Error).message}
          </div>
        ) : null}
      </div>
    </Modal>
  );
}
