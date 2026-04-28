import type { TodoFilter } from '../types/sticky';

// 把 React Query 的 key 全部汇总在一处，避免四处散落导致 invalidate 时遗漏。
//
// 约定：
//   - todos / todo / tags / stickies 作为"资源类"key 的第一段；
//     useRealtimeSync 收到 WS 事件后用 invalidateQueries({queryKey: ['todos']})
//     这种"前缀 match" 的方式精准刷新，不必关心具体 filter/id
//   - 事件 → invalidate key 映射：
//       todo.*   → ['todos'] + ['tags']（新增/删除 todo 可能影响 tag 汇总）
//       sticky.* → ['stickies']
export const queryKeys = {
  todos: (filter: TodoFilter) => ['todos', filter] as const,
  todo: (id: number) => ['todo', id] as const,
  todoHistory: (todoId: number, page: number) =>
    ['todo-history', todoId, page] as const,
  auditLogs: (params: { page: number; pageSize: number; action?: string }) =>
    ['audit-logs', params] as const,
  tags: () => ['tags'] as const,
  stickies: () => ['stickies'] as const,
  health: () => ['health'] as const,
};
