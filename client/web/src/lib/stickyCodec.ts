// 便签 DTO 与运行时视图模型之间的双向转换。
//
// 背景：后端 StickyNote.bg_color / filter 是**字符串形式的 JSON**（见
// server/internal/model/models.go：frame / bg_color / filter 三个 TEXT 列，
// 服务端只做 json.Valid + 长度校验，不解析内部结构）。
// Web 端 UI 消费的是 hex 颜色（#RRGGBB）和 TodoFilter 对象，因此 API 层
// 需要把 DTO 解出来给 UI 用，又把 UI 的修改重新打包成 DTO 发给后端。
//
// 设计原则：
//   1) 解码永不抛错——任何损坏/缺字段的 DTO 都通过兜底值还原成合法的 StickyView；
//      这样即使后端未来扩展字段或某条记录历史上被写脏，UI 也不会整面崩。
//   2) 编码永远产出合法 JSON 字符串——交给后端的 bg_color / filter 必然能通过
//      json.Valid 校验，避免跨端联调踩到 400 Bad Request。
//   3) hex ↔ RGBA 的语义与 macOS 客户端 CodableRGBA 完全一致（sRGB，分量 0..1），
//      保证 Web 写入的颜色 macOS 读得到、反之亦然。

import { hexToRgb } from './color';
import {
  DEFAULT_STICKY_COLOR,
  defaultFilter,
  type StickyNoteDTO,
  type StickyView,
  type TodoFilter,
  type UpsertStickyRequest,
} from '../types/sticky';

/** 与 macOS `CodableRGBA` 对齐的颜色对象。分量均在 [0,1] 的 sRGB 空间。 */
interface CodableRGBA {
  red: number;
  green: number;
  blue: number;
  alpha: number;
}

/**
 * 把 hex（#RRGGBB）转成 CodableRGBA JSON 字符串。
 *
 * 兜底链路：hexToRgb 内部对 null / 空串 / 非法格式统一回退为 (1,1,1)（白色），
 * 所以这里不做二次兜底——上游语义（如"找不到颜色回退到默认便签黄"）应由调用方
 * 显式传 DEFAULT_STICKY_COLOR 表达，不在 codec 做隐式转换。
 *
 * alpha 恒为 1：便签 UI 不支持半透明，强制不透明可避免跨端出现"某端读到 alpha=0
 * 导致便签整体变透明" 的隐性脏数据。
 */
export function hexToBgColorJSON(hex: string | null | undefined): string {
  const { r, g, b } = hexToRgb(hex);
  const payload: CodableRGBA = { red: r, green: g, blue: b, alpha: 1 };
  return JSON.stringify(payload);
}

/**
 * 把后端 bg_color 的 JSON 字符串解码成 hex（#RRGGBB）。
 *
 * 兜底策略（任何一步失败就返回 DEFAULT_STICKY_COLOR）：
 *   - 空串 / null / undefined
 *   - JSON 解析失败
 *   - 结构不匹配（缺字段 / 类型错 / NaN / 分量越界）
 */
export function bgColorJSONToHex(raw: string | null | undefined): string {
  if (typeof raw !== 'string' || raw.trim().length === 0) {
    return DEFAULT_STICKY_COLOR;
  }
  let parsed: unknown;
  try {
    parsed = JSON.parse(raw);
  } catch {
    return DEFAULT_STICKY_COLOR;
  }
  if (!parsed || typeof parsed !== 'object') {
    return DEFAULT_STICKY_COLOR;
  }
  const obj = parsed as Partial<CodableRGBA>;
  const r = normalizeChannel(obj.red);
  const g = normalizeChannel(obj.green);
  const b = normalizeChannel(obj.blue);
  if (r === null || g === null || b === null) {
    return DEFAULT_STICKY_COLOR;
  }
  return rgbToHex(r, g, b);
}

/**
 * 把 TodoFilter 序列化成后端期望的 JSON 字符串。
 *
 * 注意：后端对 filter 只做 json.Valid，不校验字段——所以这里传 TodoFilter 整体
 * 即可，即使前端以后给 TodoFilter 加字段，后端也不需要同步。
 */
export function filterToJSON(filter: TodoFilter): string {
  return JSON.stringify(filter);
}

/**
 * 解码后端 filter 字符串为 TodoFilter 对象。
 *
 * 兜底：任何异常都用 defaultFilter 兜住，并与已解析出的字段浅合并。
 * 这样 UI 组件拿到的永远是"必字段齐全的 TodoFilter"，不会因读到历史脏数据而崩。
 */
export function filterJSONToObject(raw: string | null | undefined): TodoFilter {
  if (typeof raw !== 'string' || raw.trim().length === 0) {
    return { ...defaultFilter };
  }
  let parsed: unknown;
  try {
    parsed = JSON.parse(raw);
  } catch {
    return { ...defaultFilter };
  }
  if (!parsed || typeof parsed !== 'object' || Array.isArray(parsed)) {
    return { ...defaultFilter };
  }
  // 浅合并：未知字段一并保留（后端透传），已知字段用 defaultFilter 补齐缺失项。
  return { ...defaultFilter, ...(parsed as Partial<TodoFilter>) };
}

/** DTO → 运行时视图。 */
export function dtoToView(dto: StickyNoteDTO): StickyView {
  return {
    id: dto.id,
    title: dto.title ?? '',
    bgColor: bgColorJSONToHex(dto.bg_color),
    filter: filterJSONToObject(dto.filter),
    createdAt: dto.created_at,
    updatedAt: dto.updated_at,
  };
}

/**
 * 把运行时视图序列化成 PUT 请求体。
 *
 * frame 恒为 "{}"：Web 不维护便签位置，后端的 frame 列保持一个合法的空对象，
 * 让 macOS 端读到后用 FrameStore 的本地值渲染。跨端约定参见 implementation_plan.md。
 */
export function viewToUpsertRequest(view: {
  title: string;
  bgColor: string;
  filter: TodoFilter;
}): UpsertStickyRequest {
  return {
    title: view.title ?? '',
    frame: '{}',
    bg_color: hexToBgColorJSON(view.bgColor),
    filter: filterToJSON(view.filter),
  };
}

// ---------- 内部工具 ----------

/**
 * 把任意输入归一化到 [0,1] 的单个颜色分量，失败返回 null。
 * 容错：允许数字、也允许可被 parseFloat 解析的字符串（后端理论上只会送数字，
 * 但历史脏数据可能是字符串；一次性在解码侧兜住更稳）。
 */
function normalizeChannel(v: unknown): number | null {
  let n: number;
  if (typeof v === 'number') {
    n = v;
  } else if (typeof v === 'string') {
    n = Number.parseFloat(v);
  } else {
    return null;
  }
  if (!Number.isFinite(n)) return null;
  if (n < 0 || n > 1) return null;
  return n;
}

/** (r,g,b) ∈ [0,1] → "#RRGGBB" */
function rgbToHex(r: number, g: number, b: number): string {
  const to255 = (x: number) => Math.round(Math.min(1, Math.max(0, x)) * 255);
  const hex = (x: number) => to255(x).toString(16).padStart(2, '0');
  return `#${hex(r)}${hex(g)}${hex(b)}`.toUpperCase();
}
