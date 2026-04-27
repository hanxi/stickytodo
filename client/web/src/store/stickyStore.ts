import { create } from 'zustand';
import { persist } from 'zustand/middleware';
import {
  DEFAULT_STICKY_COLOR,
  defaultFilter,
  type StickyNote,
  type TodoFilter,
} from '../types/sticky';

interface StickyState {
  stickies: StickyNote[];
  addSticky: (overrides?: Partial<Omit<StickyNote, 'id' | 'createdAt'>>) => StickyNote;
  removeSticky: (id: string) => void;
  updateSticky: (id: string, patch: Partial<Omit<StickyNote, 'id'>>) => void;
  updateFilter: (id: string, patch: Partial<TodoFilter>) => void;
  replaceFilter: (id: string, filter: TodoFilter) => void;
  ensureDefault: () => void;
}

function randomId(): string {
  if (typeof crypto !== 'undefined' && 'randomUUID' in crypto) {
    return crypto.randomUUID();
  }
  return `sticky-${Date.now().toString(36)}-${Math.random().toString(36).slice(2, 10)}`;
}

function createSticky(overrides: Partial<Omit<StickyNote, 'id' | 'createdAt'>> = {}): StickyNote {
  return {
    id: randomId(),
    title: overrides.title ?? '新便签',
    color: overrides.color ?? DEFAULT_STICKY_COLOR,
    filter: overrides.filter ?? { ...defaultFilter },
    createdAt: Date.now(),
  };
}

// 兜底补齐一条便签的必填字段，防止老版本 localStorage 里的数据缺字段（比如
// 早期版本的 StickyNote 没有 color/filter）。在 rehydrate 阶段统一跑一遍，
// 避免组件渲染时读到 undefined 再炸（例如 hexToRgb(undefined)）。
function normalizeSticky(raw: Partial<StickyNote> | null | undefined): StickyNote | null {
  if (!raw || typeof raw !== 'object') return null;
  const id = typeof raw.id === 'string' && raw.id.length > 0 ? raw.id : randomId();
  const title = typeof raw.title === 'string' ? raw.title : '新便签';
  const color =
    typeof raw.color === 'string' && raw.color.length > 0
      ? raw.color
      : DEFAULT_STICKY_COLOR;
  const filter: TodoFilter = {
    ...defaultFilter,
    ...(raw.filter && typeof raw.filter === 'object' ? raw.filter : {}),
  };
  const createdAt =
    typeof raw.createdAt === 'number' && Number.isFinite(raw.createdAt)
      ? raw.createdAt
      : Date.now();
  return { id, title, color, filter, createdAt };
}

export const useStickyStore = create<StickyState>()(
  persist(
    (set, get) => ({
      stickies: [],
      addSticky: (overrides) => {
        const sticky = createSticky(overrides);
        set({ stickies: [...get().stickies, sticky] });
        return sticky;
      },
      removeSticky: (id) =>
        set({ stickies: get().stickies.filter((n) => n.id !== id) }),
      updateSticky: (id, patch) =>
        set({
          stickies: get().stickies.map((n) => (n.id === id ? { ...n, ...patch } : n)),
        }),
      updateFilter: (id, patch) =>
        set({
          stickies: get().stickies.map((n) =>
            n.id === id ? { ...n, filter: { ...n.filter, ...patch } } : n,
          ),
        }),
      replaceFilter: (id, filter) =>
        set({
          stickies: get().stickies.map((n) => (n.id === id ? { ...n, filter } : n)),
        }),
      ensureDefault: () => {
        if (get().stickies.length === 0) {
          set({ stickies: [createSticky({ title: '默认便签' })] });
        }
      },
    }),
    {
      name: 'stickytodo.stickies',
      version: 1,
      // 在从 localStorage 读回之后、替换 store 状态之前规整每条 sticky，
      // 保证每一条都至少有 color / filter，避免渲染时拿到 undefined。
      merge: (persisted, current) => {
        const raw = persisted as { stickies?: unknown } | undefined;
        const list = Array.isArray(raw?.stickies) ? raw!.stickies : [];
        const normalized = list
          .map((item) => normalizeSticky(item as Partial<StickyNote>))
          .filter((x): x is StickyNote => x !== null);
        return { ...current, stickies: normalized };
      },
    },
  ),
);
