import type { TodoStatus } from './api';

// 便签内部的筛选条件，字段名与后端 /api/todos?query 的查询参数完全对齐。
export interface TodoFilter {
  status?: TodoStatus | 'all';
  tag: string;
  keyword: string;
  due_before?: string; // RFC3339
  include_deleted: boolean;
  only_deleted: boolean;
  page: number;
  page_size: number;
}

export const defaultFilter: TodoFilter = {
  status: 'all',
  tag: '',
  keyword: '',
  include_deleted: false,
  only_deleted: false,
  page: 1,
  page_size: 50,
};

export interface StickyColor {
  name: string;
  hex: string; // #RRGGBB
}

// 与 macOS 客户端 CodableRGBA 预设色一致（sRGB）。
export const STICKY_COLORS: StickyColor[] = [
  { name: '便签黄', hex: '#FFEB8A' },
  { name: '薄荷绿', hex: '#C8F1C8' },
  { name: '天空蓝', hex: '#C6E3FF' },
  { name: '樱花粉', hex: '#FFD1DC' },
  { name: '薰衣草', hex: '#E1D4FF' },
];

export const DEFAULT_STICKY_COLOR = STICKY_COLORS[0]!.hex;

export interface StickyNote {
  id: string; // crypto.randomUUID()
  title: string;
  color: string; // hex
  filter: TodoFilter;
  createdAt: number; // Date.now()
}
