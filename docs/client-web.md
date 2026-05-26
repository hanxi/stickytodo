# Web 客户端（client/web/）

技术栈：**React 18 + Vite 5 + TypeScript + Tailwind 3 + Zustand 4 + TanStack Query v5 + date-fns 3 + lucide-react**（版本见 `client/web/package.json`）

---

## 核心抽象

### API 层（`src/api/`）

- **`client.ts`**：`fetch` 薄封装，统一注入 `Authorization: Bearer`、401 自动 logout、`ApiError` 结构化错误。所有请求经由单个 `request<T>()` 函数，路径前缀是**空字符串**（生产同源 `/api`，开发 Vite proxy 转发到 8080）。新增的 sticky 方法（`listStickies` / `upsertSticky` / `deleteSticky` / `getSticky`）在 `client.ts` 内部通过 `hexToBgColorJSON` / `filterToJSON`（见 `src/lib/stickyCodec.ts`）完成"hex 颜色 ↔ CodableRGBA JSON"和"TodoFilter 对象 ↔ 字符串"的双向编解码，上层组件只需消费 `StickyView`
- **`queryKeys.ts`**：TanStack Query cache key 集中管理，包含 `qk.stickies()`
- **`ws.ts`**：原生 `WebSocket` 单例客户端 `stickyWS`。实现了首帧 auth、指数退避 `[1,2,4,8,16,30]s`、`visibilitychange` 立即重连、`close code 4401 → 'unauthorized' signal` 等完整协议逻辑；业务层通过 `onEvent` / `onSignal` 订阅

### 状态层（`src/store/`）

实际只有两个 store（`stickyStore.ts` 已在云端数据源重构中删除，便签数据改由 TanStack Query 管理）：

- **`authStore.ts`**：JWT token + username，通过 `zustand/persist` 存 `localStorage`
- **`uiStore.ts`**：深色模式偏好，`type DarkMode = 'system' | 'light' | 'dark'`（注意 system 排第一个，枚举真值以代码为准）

### Hook 层（`src/hooks/`）

- **`useRealtimeSync.ts`**：桥接 `stickyWS` 与 TanStack Query cache。监听 `authStore.token` 变化控制连接；收到 `todo.*` 事件 → `queryClient.invalidateQueries({queryKey: qk.todos(...)})`；收到 `sticky.*` 事件 → invalidate `qk.stickies()`；收到 `'reconnected'` signal → 全量 invalidate；收到 `'unauthorized'` → 调用 `authStore.logout()`

### 工具层（`src/lib/`）

- **`color.ts`**：`hexToRgb` / `luminance` / `isLightBackground` / `priorityColor` / `foregroundFor`，全部入参 `string | null | undefined` 容错
- **`format.ts`**：`formatDue` / `formatRelative` / `filterSummary` / `toISOFromLocalInput` / `toLocalInputFromISO`，依赖 date-fns
- **`stickyCodec.ts`**：`StickyNoteDTO ↔ StickyView` 双向转换；解码侧所有异常都用 `DEFAULT_STICKY_COLOR` / `defaultFilter` 兜底（脏数据不阻塞 UI），编码侧保证输出合法 JSON（后端 `json.Valid` 校验必过）；`viewToUpsertRequest` 里 `frame` 恒 `"{}"`

### 服务端状态（TanStack Query）

所有远端数据都经 `useQuery` / `useMutation`，cache key 由 `queryKeys.ts` 集中管理；便签列表也由 `useQuery(qk.stickies(), api.listStickies)` 订阅，与 TODO 数据走同一条缓存链路。

### 视图

`views/StickyBoard` → `components/StickyCard` → `components/TodoList` → `components/TodoRow`，一张便签就是一个过滤器，多张便签可以订阅不同筛选条件并排放；`AppBar` 提供全局历史入口 + "新建便签"（通过 `useMutation(api.upsertSticky)` 乐观更新）；`HistoryView` 是审计日志弹窗、`EditTodoSheet` / `FilterEditor` / `DraftTodoRow` / `Modal` 是配套交互组件。`App.tsx` 在根部挂载 `useRealtimeSync()` hook 统一驱动 WS。

---

## Vite 关键配置

- `base: '/app/'`（和后端 embed 挂载路径一致）
- `build.outDir: 'dist'`
- dev 时 proxy `/api`、`/health` 到 `127.0.0.1:8080`

### dev 模式下 WebSocket 的已知约束

`stickyWS` 用 `window.location.origin` 翻译成 `ws(s)://host/api/ws`——生产环境同源（`/app/` 和 `/api/ws` 共享 `window.location.host`）正确；但 `vite.config.ts` 的 `server.proxy['/api']` 目前**没有显式配 `ws: true`**，`http-proxy-middleware` 在缺省 `ws` 选项时**不会**代理 WebSocket 升级请求。所以开发时前端跑在 5173、后端跑在 8080 的情况下，WS 握手会被 Vite dev server 以 404 拒绝。

**临时绕过办法**：在本地给 `ws.ts` 的 `connect(token, baseURL)` 第二参数显式传 `'http://127.0.0.1:8080'`，让 WS 直连后端。

**长期方案**：给 vite.config 的 `/api` proxy 加 `ws: true`（此改动不在本重构范围内，按需独立提 PR）。
