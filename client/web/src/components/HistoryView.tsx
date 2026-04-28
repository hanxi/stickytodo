import { useQuery } from '@tanstack/react-query';
import Modal from './Modal';
import { api } from '../api/client';
import { queryKeys } from '../api/queryKeys';
import { formatRelative } from '../lib/format';
import type { AuditLog } from '../types/api';
import { useEffect, useState } from 'react';

export type HistoryMode =
  | { kind: 'global' }
  | { kind: 'todo'; todoId: number; title: string };

interface Props {
  mode: HistoryMode;
  onClose: () => void;
}

// 后端 audit_logs.detail 字段的已知 shape（见 server/internal/service/todo_service.go
// 与 sticky_service.go）：
//   - todo.create / sticky_upsert：{"after": {...}} (sticky_upsert 还带 sticky_id)
//   - todo.update：{"changed": {"<field>": {"before": v1, "after": v2}, ...}}
//   - todo.complete / todo.reopen：{"before": {...}, "after": {...}}
//   - todo.delete / todo.restore / sticky_delete / login*：自由 map
// 为了健壮，这里全部用 unknown + 运行时判断，缺字段/类型不符时自动退化到原始 JSON。
type DetailShape = {
  before?: unknown;
  after?: unknown;
  changed?: Record<string, { before?: unknown; after?: unknown }>;
  [k: string]: unknown;
};

function parseDetail(raw: string | null | undefined): DetailShape | null {
  if (!raw) return null;
  try {
    const obj = JSON.parse(raw);
    if (obj && typeof obj === 'object' && !Array.isArray(obj)) {
      return obj as DetailShape;
    }
    return null;
  } catch {
    return null;
  }
}

function formatValue(v: unknown): string {
  if (v === null || v === undefined) return '∅';
  if (typeof v === 'string') return v === '' ? '""' : v;
  if (typeof v === 'number' || typeof v === 'boolean') return String(v);
  // 对象 / 数组 / 其它：序列化，保留可读性
  try {
    return JSON.stringify(v);
  } catch {
    return String(v);
  }
}

function prettyJson(raw: string | null | undefined): string {
  if (!raw) return '—';
  try {
    return JSON.stringify(JSON.parse(raw), null, 2);
  } catch {
    return raw;
  }
}

function DetailBody({ log }: { log: AuditLog }) {
  const detail = parseDetail(log.detail);

  // detail 为空串 / 非对象 / 非法 JSON：兜底展示原始字符串
  if (!detail) {
    return (
      <pre className="max-h-40 overflow-auto rounded bg-gray-50 p-1 text-[10px] dark:bg-neutral-900/50">
        {log.detail ? log.detail : '—'}
      </pre>
    );
  }

  // 1) update 的 changed 字段：按字段展示变更
  const changed = detail.changed;
  if (changed && typeof changed === 'object' && !Array.isArray(changed)) {
    const entries = Object.entries(changed);
    if (entries.length > 0) {
      return (
        <div className="space-y-1">
          <div className="text-gray-500">changed</div>
          <div className="overflow-auto rounded border border-gray-200 dark:border-neutral-700">
            <table className="w-full text-[10px]">
              <thead className="bg-gray-50 dark:bg-neutral-900/50">
                <tr>
                  <th className="px-2 py-1 text-left font-normal text-gray-500">field</th>
                  <th className="px-2 py-1 text-left font-normal text-gray-500">before</th>
                  <th className="px-2 py-1 text-left font-normal text-gray-500">after</th>
                </tr>
              </thead>
              <tbody>
                {entries.map(([field, diff]) => (
                  <tr key={field} className="border-t border-gray-100 dark:border-neutral-800">
                    <td className="px-2 py-1 font-mono">{field}</td>
                    <td className="px-2 py-1 text-red-600 dark:text-red-400">
                      {formatValue(diff?.before)}
                    </td>
                    <td className="px-2 py-1 text-green-600 dark:text-green-400">
                      {formatValue(diff?.after)}
                    </td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        </div>
      );
    }
  }

  // 2) 双快照（complete / reopen）：before + after 双列对照
  if (detail.before !== undefined && detail.after !== undefined) {
    return (
      <div className="grid grid-cols-2 gap-2">
        <div>
          <div className="mb-1 text-gray-500">before</div>
          <pre className="max-h-40 overflow-auto rounded bg-gray-50 p-1 text-[10px] dark:bg-neutral-900/50">
            {JSON.stringify(detail.before, null, 2)}
          </pre>
        </div>
        <div>
          <div className="mb-1 text-gray-500">after</div>
          <pre className="max-h-40 overflow-auto rounded bg-gray-50 p-1 text-[10px] dark:bg-neutral-900/50">
            {JSON.stringify(detail.after, null, 2)}
          </pre>
        </div>
      </div>
    );
  }

  // 3) 仅 after（create / sticky_upsert）：单列展示
  if (detail.after !== undefined) {
    return (
      <div>
        <div className="mb-1 text-gray-500">after</div>
        <pre className="max-h-40 overflow-auto rounded bg-gray-50 p-1 text-[10px] dark:bg-neutral-900/50">
          {JSON.stringify(detail.after, null, 2)}
        </pre>
      </div>
    );
  }

  // 4) 其它（login / login_failed / delete / restore / sticky_delete 等）：
  //    detail 是自由 map，格式化整块即可
  return (
    <div>
      <div className="mb-1 text-gray-500">detail</div>
      <pre className="max-h-40 overflow-auto rounded bg-gray-50 p-1 text-[10px] dark:bg-neutral-900/50">
        {prettyJson(log.detail)}
      </pre>
    </div>
  );
}

function AuditRow({ log }: { log: AuditLog }) {
  const [expanded, setExpanded] = useState(false);
  return (
    <div className="rounded border border-gray-200 p-2 text-xs dark:border-neutral-700">
      <div className="flex items-center justify-between">
        <div className="flex items-center gap-2">
          <span className="font-mono">{log.action}</span>
          <span className="text-gray-500">by {log.actor}</span>
          {log.todo_id !== null ? (
            <span className="text-gray-400">#todo {log.todo_id}</span>
          ) : null}
        </div>
        <div className="flex items-center gap-2 text-gray-500">
          <span>{formatRelative(log.created_at)}</span>
          <button
            type="button"
            onClick={() => setExpanded((v) => !v)}
            className="rounded px-1 hover:bg-gray-100 dark:hover:bg-neutral-700"
          >
            {expanded ? '收起' : '详情'}
          </button>
        </div>
      </div>
      {expanded ? (
        <div className="mt-2">
          <DetailBody log={log} />
        </div>
      ) : null}
    </div>
  );
}

export default function HistoryView({ mode, onClose }: Props) {
  const [page, setPage] = useState(1);
  const pageSize = 20;

  // mode 切换时重置分页，避免进入一个空页码的怪异状态。
  // 依赖放 mode.kind 和 todoId：切到全局、切到另一条 todo 都会重置。
  const modeKey = mode.kind === 'global' ? 'global' : `todo:${mode.todoId}`;
  useEffect(() => {
    setPage(1);
  }, [modeKey]);

  // 必须用单个 useQuery，禁止把 useQuery 放到条件分支里——那会违反
  // Rules of Hooks：mode 切换时 hook 调用顺序错位，React 会把 query state
  // 指向错误的 slot，导致网络请求发出但组件拿到 undefined data，页面显示
  // "暂无记录"（现象就是这次修的 bug）。
  const query = useQuery({
    queryKey:
      mode.kind === 'global'
        ? queryKeys.auditLogs({ page, pageSize })
        : queryKeys.todoHistory(mode.todoId, page),
    queryFn: () =>
      mode.kind === 'global'
        ? api.listAuditLogs({ page, pageSize })
        : api.listTodoHistory(mode.todoId, page, pageSize),
  });

  const title =
    mode.kind === 'global' ? '全局审计日志' : `历史：${mode.title}`;

  const items = query.data?.items ?? [];
  const total = query.data?.total ?? 0;
  const maxPage = Math.max(1, Math.ceil(total / pageSize));

  return (
    <Modal
      open
      onClose={onClose}
      title={title}
      maxWidth="max-w-3xl"
      footer={
        <>
          <button
            type="button"
            className="rounded px-2 py-1 text-xs hover:bg-gray-100 dark:hover:bg-neutral-700"
            disabled={page <= 1}
            onClick={() => setPage((p) => Math.max(1, p - 1))}
          >
            上一页
          </button>
          <span className="text-xs text-gray-500">
            第 {page} / {maxPage} 页 · 共 {total} 条
          </span>
          <button
            type="button"
            className="rounded px-2 py-1 text-xs hover:bg-gray-100 dark:hover:bg-neutral-700"
            disabled={page >= maxPage}
            onClick={() => setPage((p) => Math.min(maxPage, p + 1))}
          >
            下一页
          </button>
          <button
            type="button"
            onClick={onClose}
            className="rounded bg-blue-600 px-3 py-1 text-xs text-white hover:bg-blue-700"
          >
            关闭
          </button>
        </>
      }
    >
      {query.isLoading ? (
        <div className="py-6 text-center text-xs text-gray-500">加载中…</div>
      ) : query.isError ? (
        <div className="py-6 text-center text-xs text-red-600">
          加载失败：{(query.error as Error).message}
        </div>
      ) : items.length === 0 ? (
        <div className="py-6 text-center text-xs text-gray-500">暂无记录</div>
      ) : (
        <div className="space-y-2">
          {items.map((log) => (
            <AuditRow key={log.id} log={log} />
          ))}
        </div>
      )}
    </Modal>
  );
}
