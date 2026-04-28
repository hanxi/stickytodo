import { useAuthStore } from '../store/authStore';
import type {
  AuditListResponse,
  CreateTodoRequest,
  LoginRequest,
  LoginResponse,
  TagListResponse,
  Todo,
  TodoHistoryResponse,
  TodoListResponse,
  UpdateTodoRequest,
} from '../types/api';
import type {
  StickyNoteDTO,
  StickyView,
  TodoFilter,
  UpsertStickyRequest,
} from '../types/sticky';
import { dtoToView, viewToUpsertRequest } from '../lib/stickyCodec';

/**
 * 后端返回 {"error": "..."} 时抛出的结构化错误。
 * status 为 HTTP 状态码，便于调用方区分 401 / 400 / 404 / 5xx。
 */
export class ApiError extends Error {
  constructor(public status: number, message: string) {
    super(message);
    this.name = 'ApiError';
  }
}

// 生产环境走同源 /api；开发环境由 vite.config.ts 的 proxy 转发到 8080。
const API_BASE = '';

async function request<T>(
  path: string,
  init: RequestInit = {},
): Promise<T> {
  const token = useAuthStore.getState().token;
  const headers = new Headers(init.headers);
  if (!headers.has('Content-Type') && init.body) {
    headers.set('Content-Type', 'application/json');
  }
  if (token) {
    headers.set('Authorization', `Bearer ${token}`);
  }

  const resp = await fetch(`${API_BASE}${path}`, { ...init, headers });

  if (resp.status === 401) {
    // token 失效：清登录态，外层会自动跳回登录页
    useAuthStore.getState().logout();
    throw new ApiError(401, 'unauthorized');
  }

  if (!resp.ok) {
    let message = `HTTP ${resp.status}`;
    try {
      const body = (await resp.json()) as { error?: string };
      if (body.error) {
        message = body.error;
      }
    } catch {
      // 忽略 json 解析失败，继续用默认消息
    }
    throw new ApiError(resp.status, message);
  }

  // 204 No Content
  if (resp.status === 204) {
    return undefined as T;
  }
  return (await resp.json()) as T;
}

function filterToQuery(filter: TodoFilter): string {
  const params = new URLSearchParams();
  if (filter.status && filter.status !== 'all') {
    params.set('status', filter.status);
  }
  if (filter.tag) {
    params.set('tag', filter.tag);
  }
  if (filter.keyword) {
    params.set('keyword', filter.keyword);
  }
  if (filter.due_before) {
    params.set('due_before', filter.due_before);
  }
  if (filter.include_deleted) {
    params.set('include_deleted', 'true');
  }
  if (filter.only_deleted) {
    params.set('only_deleted', 'true');
  }
  params.set('page', String(filter.page));
  params.set('page_size', String(filter.page_size));
  return params.toString();
}

export const api = {
  health: () => request<{ status: string }>('/health'),

  login: (body: LoginRequest) =>
    request<LoginResponse>('/api/login', {
      method: 'POST',
      body: JSON.stringify(body),
    }),

  listTodos: (filter: TodoFilter) =>
    request<TodoListResponse>(`/api/todos?${filterToQuery(filter)}`),

  getTodo: (id: number) => request<Todo>(`/api/todos/${id}`),

  createTodo: (body: CreateTodoRequest) =>
    request<Todo>('/api/todos', {
      method: 'POST',
      body: JSON.stringify(body),
    }),

  updateTodo: (id: number, body: UpdateTodoRequest) =>
    request<Todo>(`/api/todos/${id}`, {
      method: 'PATCH',
      body: JSON.stringify(body),
    }),

  completeTodo: (id: number) =>
    request<Todo>(`/api/todos/${id}/complete`, { method: 'POST' }),

  reopenTodo: (id: number) =>
    request<Todo>(`/api/todos/${id}/reopen`, { method: 'POST' }),

  deleteTodo: (id: number) =>
    request<void>(`/api/todos/${id}`, { method: 'DELETE' }),

  restoreTodo: (id: number) =>
    request<Todo>(`/api/todos/${id}/restore`, { method: 'POST' }),

  listTodoHistory: (id: number, page: number, pageSize: number) =>
    request<TodoHistoryResponse>(
      `/api/todos/${id}/history?page=${page}&page_size=${pageSize}`,
    ),

  listAuditLogs: (params: { page: number; pageSize: number; action?: string }) => {
    const q = new URLSearchParams();
    q.set('page', String(params.page));
    q.set('page_size', String(params.pageSize));
    if (params.action) {
      q.set('action', params.action);
    }
    return request<AuditListResponse>(`/api/audit-logs?${q.toString()}`);
  },

  listTags: () => request<TagListResponse>('/api/tags'),

  // ---------- Sticky Notes ----------
  //
  // 后端路由（见 server/internal/router/router.go）：
  //   GET    /api/sticky-notes        → 返回 { items: StickyNoteDTO[] }
  //   GET    /api/sticky-notes/:id    → 返回 StickyNoteDTO
  //   PUT    /api/sticky-notes/:id    → body=UpsertStickyRequest，返回 StickyNoteDTO
  //   DELETE /api/sticky-notes/:id    → 返回 { id, deleted: true }
  //
  // 这一层职责：把 DTO ↔ StickyView 的转换收口，上层组件只感知 StickyView。
  // 删除接口不返回 view（调用方只需要知道删没删成功），直接用 200 即可。

  listStickies: async (): Promise<StickyView[]> => {
    const resp = await request<{ items: StickyNoteDTO[] }>('/api/sticky-notes');
    // 后端即使无数据也会返回 items:[]（见 handler.List），这里不做 null 兜底以暴露协议变更
    return resp.items.map(dtoToView);
  },

  getSticky: async (id: string): Promise<StickyView> => {
    const dto = await request<StickyNoteDTO>(
      `/api/sticky-notes/${encodeURIComponent(id)}`,
    );
    return dtoToView(dto);
  },

  /**
   * 幂等 upsert。入参是 StickyView 的"可写子集"（id + title + bgColor + filter），
   * 内部负责序列化成 UpsertStickyRequest 再 PUT。
   *
   * 服务端会用自己的时间戳覆写 created_at/updated_at，所以出参以服务端为准。
   */
  upsertSticky: async (input: {
    id: string;
    title: string;
    bgColor: string;
    filter: TodoFilter;
  }): Promise<StickyView> => {
    const body: UpsertStickyRequest = viewToUpsertRequest({
      title: input.title,
      bgColor: input.bgColor,
      filter: input.filter,
    });
    const dto = await request<StickyNoteDTO>(
      `/api/sticky-notes/${encodeURIComponent(input.id)}`,
      {
        method: 'PUT',
        body: JSON.stringify(body),
      },
    );
    return dtoToView(dto);
  },

  deleteSticky: (id: string) =>
    request<{ id: string; deleted: boolean }>(
      `/api/sticky-notes/${encodeURIComponent(id)}`,
      { method: 'DELETE' },
    ),
};
