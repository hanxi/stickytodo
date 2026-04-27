import { format, formatDistanceToNowStrict, isValid, parseISO } from 'date-fns';
import type { TodoFilter } from '../types/sticky';

export function formatDue(iso: string | null): string {
  if (!iso) return '';
  const d = parseISO(iso);
  if (!isValid(d)) return '';
  return format(d, 'yyyy-MM-dd HH:mm');
}

export function formatRelative(iso: string): string {
  const d = parseISO(iso);
  if (!isValid(d)) return iso;
  return formatDistanceToNowStrict(d, { addSuffix: true });
}

/** 单句筛选摘要，用于便签底部显示。 */
export function filterSummary(filter: TodoFilter): string {
  const parts: string[] = [];
  if (filter.status && filter.status !== 'all') {
    parts.push(filter.status === 'pending' ? '未完成' : '已完成');
  }
  if (filter.tag) parts.push(`标签=${filter.tag}`);
  if (filter.keyword) parts.push(`关键词=${filter.keyword}`);
  if (filter.due_before) {
    parts.push(`截止前 ${formatDue(filter.due_before)}`);
  }
  if (filter.only_deleted) {
    parts.push('仅看已删除');
  } else if (filter.include_deleted) {
    parts.push('含已删除');
  }
  return parts.length ? parts.join(' · ') : '全部';
}

/** 将 datetime-local 表单值（无时区）转成 RFC3339（本地时区）。 */
export function toISOFromLocalInput(localValue: string): string | undefined {
  if (!localValue) return undefined;
  const d = new Date(localValue);
  if (!isValid(d)) return undefined;
  return d.toISOString();
}

/** 将 RFC3339 转成 <input type="datetime-local"> 可回填的值。 */
export function toLocalInputFromISO(iso: string | null | undefined): string {
  if (!iso) return '';
  const d = parseISO(iso);
  if (!isValid(d)) return '';
  // yyyy-MM-ddTHH:mm（不含秒/时区，正是 datetime-local 的格式）
  const pad = (n: number) => String(n).padStart(2, '0');
  return (
    d.getFullYear() +
    '-' +
    pad(d.getMonth() + 1) +
    '-' +
    pad(d.getDate()) +
    'T' +
    pad(d.getHours()) +
    ':' +
    pad(d.getMinutes())
  );
}
