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

/**
 * StickyNoteDTO —— 与后端 GORM model.StickyNote 的 JSON 序列化**精确对齐**。
 *
 * 字段由 server/internal/model/models.go 定义：
 *   id / title / frame / bg_color / filter / created_at / updated_at
 *
 * 注意：
 *  - frame / bg_color / filter 都是**字符串形式的 JSON**（后端对这三个字段只做
 *    json.Valid + 长度校验，不解析内部 schema）。
 *  - Web 端不关心 frame（便签位置由 macOS 客户端独立持久化，Web 是同源流式 UI，
 *    不存在"窗口位置"概念），但 PUT 请求体里仍必须带 frame，否则跨端语义不一致。
 *    Web 统一以 "{}" 发送。
 *  - bg_color 后端是 {red,green,blue,alpha}（0..1 的 sRGB 分量）的 JSON 字符串；
 *    Web UI 使用 hex 字符串（#RRGGBB）更便于 Tailwind / style 引用，因此需要
 *    stickyCodec 做双向转换（见 lib/stickyCodec.ts）。
 *  - deleted_at 后端软删后不返回（List/Get 过滤掉），Web 端不持有。
 */
export interface StickyNoteDTO {
  id: string;
  title: string;
  frame: string;    // JSON 字符串；Web 恒为 "{}"
  bg_color: string; // JSON 字符串，形如 '{"red":1,"green":0.92,"blue":0.54,"alpha":1}'
  filter: string;   // JSON 字符串，形如 TodoFilter 序列化
  created_at: string; // RFC3339
  updated_at: string; // RFC3339
}

/**
 * StickyView —— Web 端组件/store 真正使用的"视图模型"。
 *
 * 与 DTO 的区别：
 *  - bgColor 直接是 hex（#RRGGBB），不再是 JSON 字符串
 *  - filter 直接是 TodoFilter 对象，不再是 JSON 字符串
 *  - 去掉 frame（Web 不关心）、去掉 snake_case 时间戳（UI 组件不直接消费字符串时间）
 *
 * 这个类型是 useQuery 的返回类型，也是 StickyCard / FilterEditor 等组件的 prop 类型。
 */
export interface StickyView {
  id: string;
  title: string;
  bgColor: string;          // hex #RRGGBB
  filter: TodoFilter;
  createdAt: string;        // RFC3339，供 UI 展示/排序
  updatedAt: string;        // RFC3339
}

/**
 * UpsertStickyRequest —— PUT /api/sticky-notes/:id 的请求体。
 *
 * 字段集与后端 handler.upsertStickyRequest 精确对齐：
 *   title + frame + bg_color + filter
 *
 * 由 api.upsertSticky 负责从 StickyView 序列化出这个结构：
 *   - frame 恒为 "{}"
 *   - bg_color 由 hex 转成 {"red","green","blue","alpha":1} 的 JSON 字符串
 *   - filter 由 TodoFilter 对象 JSON.stringify 得到
 */
export interface UpsertStickyRequest {
  title: string;
  frame: string;
  bg_color: string;
  filter: string;
}
