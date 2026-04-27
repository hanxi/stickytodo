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
import type { TodoFilter } from '../types/sticky';

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
};
