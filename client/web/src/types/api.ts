// 与后端 server/internal/model/models.go 对齐的 DTO。
// 字段名/枚举值/日期格式必须精确一致，否则后端反序列化会报 400。

export type TodoStatus = 'pending' | 'done';

export interface Todo {
  id: number;
  title: string;
  content: string;
  status: TodoStatus;
  priority: number; // 0..3
  tag: string;
  due_at: string | null; // RFC3339 或 null
  created_at: string;
  updated_at: string;
  completed_at: string | null;
  deleted_at: string | null;
}

export interface TodoListResponse {
  items: Todo[];
  total: number;
  page: number;
  page_size: number;
}

export interface CreateTodoRequest {
  title: string;
  content?: string;
  priority?: number;
  tag?: string;
  due_at?: string;
}

export interface UpdateTodoRequest {
  title?: string;
  content?: string;
  priority?: number;
  tag?: string;
  due_at?: string;
  clear_due_at?: boolean;
  status?: TodoStatus;
}

export interface LoginRequest {
  username: string;
  password: string;
}

export interface LoginResponse {
  token: string;
  expires_at: string;
  username: string;
}

export interface TagListResponse {
  tags: string[];
}

export type AuditAction =
  | 'login_success'
  | 'login_failure'
  | 'todo.create'
  | 'todo.update'
  | 'todo.complete'
  | 'todo.reopen'
  | 'todo.delete'
  | 'todo.restore'
  | 'sticky_upsert'
  | 'sticky_delete';

export interface AuditLog {
  id: number;
  actor: string;
  action: AuditAction | string;
  todo_id: number | null;
  before: string | null;
  after: string | null;
  occurred_at: string;
}

export interface AuditListResponse {
  items: AuditLog[];
  total: number;
  page: number;
  page_size: number;
}

export interface TodoHistoryResponse {
  items: AuditLog[];
  total: number;
  page: number;
  page_size: number;
}

export interface ErrorResponse {
  error: string;
}
