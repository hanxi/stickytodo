import type { TodoFilter } from '../types/sticky';

// 把 React Query 的 key 全部汇总在一处，避免四处散落导致 invalidate 时遗漏。
export const queryKeys = {
  todos: (filter: TodoFilter) => ['todos', filter] as const,
  todo: (id: number) => ['todo', id] as const,
  todoHistory: (todoId: number, page: number) =>
    ['todo-history', todoId, page] as const,
  auditLogs: (params: { page: number; pageSize: number; action?: string }) =>
    ['audit-logs', params] as const,
  tags: () => ['tags'] as const,
  health: () => ['health'] as const,
};
