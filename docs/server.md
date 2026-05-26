# 后端架构（server/）

详见 [AGENTS.md](../AGENTS.md) 全局视图 / 仓库目录。

---

## 分层

典型的 Repository → Service → Handler 三层，依赖方向单向：

```
router ─▶ handler ─▶ service ─▶ repository ─▶ model/GORM ─▶ *gorm.DB (SQLite)
   │
   ▼
middleware.Auth ─▶ service.AuthService (JWT 验证；不接触 gorm.DB)
```

- **model**：只定义 GORM struct 和 DB 初始化。`db.go` 里选驱动、开连接、跑 AutoMigrate、做 `PRAGMA` 调优。
- **repository**：不关心任何业务规则，只给 service 暴露"存 / 取 / 软删 / 分页"之类的原子操作。入参出参尽量是 model 结构，不暴露 `*gorm.DB`。
- **service**：业务逻辑和权限检查都在这一层。跨表事务、审计日志插入、过期检查等都是 service 的事。
- **handler**：只做三件事——解析 Gin 请求 → 调 service → 格式化响应 JSON。**不要**在 handler 里写业务规则。
- **middleware**：目录下仅有 `auth.go`，只负责解析 `Authorization: Bearer` 头、校验 JWT、把 `actor` 注入 `gin.Context`。**CORS 并不在 middleware 包**，而是 `router.go` 里的本地函数 `corsMiddleware(origins)`；**统一错误响应格式也不是 middleware 做的**，而是由各 handler 自己 `c.JSON(status, gin.H{"error": msg})` 手写，约定而非强制。
- **router**：`Build(deps *Deps) (*gin.Engine, error)` 一个函数组装所有路由。`Deps` 聚合了 services、handlers、CORS origins、Logger、Version 等注入项，`Deps.Validate()` 统一检查非空依赖。

---

## 关键文件速查

| 文件 | 作用 | 改它时要注意 |
|---|---|---|
| `server/internal/config/config.go` | 所有 `TODO_*` 环境变量解析 + 校验（Port / Username / Password / DataDir / TokenTTL / GinMode / Verbose） | 加新变量时：①新增字段 + getter 默认值 ②在 `Validate` 里校验 ③同步 `.env.example` 和根 README 的配置表 |
| `server/cmd/todo-server/main.go` | 进程入口；读 `TODO_CORS_ORIGINS` 填 router `Deps.CorsOrigins`；带 `-version` 子命令；支持 `-port` / `-username` / `-password` flag 覆盖同名环境变量（flag 非空时优先） | `TODO_CORS_ORIGINS` 不在 `config.go` 而在这里 `parseCorsOrigins`；三个 override flag 通过 `applyFlagOverride` 用 `os.Setenv` 注入，之后仍走 `config.Load` 统一校验，避免校验分叉 |
| `server/internal/model/db.go` | SQLite 驱动选择、DSN 拼装（`_pragma=...`）、AutoMigrate | 改驱动会影响 Dockerfile / CI 是否需要 CGO |
| `server/internal/model/models.go` | 所有 GORM 模型 + JSON tag | 字段改动必须同步 `client/web/src/types/api.ts` 和 Swift 的 `Models/`（详见 [dev-notes.md → 四端字段必须同步](dev-notes.md#四端字段必须同步)） |
| `server/internal/service/auth_service.go` | JWT 签发 + 校验；首次启动生成 32 字节熵、hex 编码落 `app_secrets` | 改签名算法或 claim 结构 = 强制所有存量 token 失效 |
| `server/internal/middleware/auth.go` | 仅一个 `Auth(*service.AuthService)`，解析 Bearer header、注入 `actor` 到 gin.Context | 校验失败统一 401 `{"error": ...}`；别在这里加其他业务逻辑 |
| `server/internal/webui/webui.go` | `//go:embed all:dist` + SPA fallback + CSP | 修改前读 [embed 约定](#embed-约定) |
| `server/internal/router/router.go` | 路由注册、`corsMiddleware` 本地函数、`/app` 的 GET/HEAD 301 | `/app → /app/` 必须 GET 和 HEAD 都注册；`Deps.CorsOrigins` 为空时不注入 CORS |
| `server/scripts/smoke.sh` | 36 步端到端冒烟（HTTP 黑盒 + Step 33-36 WS 回归），本项目**唯一**回归工具；启动时会 `go build ./scripts/ws-probe` 到 mktemp 作为 WS 探针 | 新增 API 或修改既有契约时必须同步加步骤，否则 CI 发不出来也发现不了；新增 WS 事件类型必须在 Step 33-36 附近加 ws-probe 校验 |
| `server/scripts/ws-probe/main.go` | `smoke.sh` 默认 `go build -o $(mktemp -d)/ws-probe ./scripts/ws-probe` 构建的 WebSocket 探针二进制；可通过导出 `WS_PROBE_BIN=/path/to/prebuilt` 环境变量复用预编译产物跳过构建（见 `smoke.sh:30` 分支）。4 种模式 `no-auth` / `bad-token` / `auth-ready` / `wait-event` 分别对应 Step 33/34/35/36；退出码 `0=pass / 1=assertion fail / 2=usage error` | 改 WS 协议帧格式（auth / ready / 事件帧）时同步改 ws-probe 的解析逻辑，否则 smoke.sh 假阳性 |
| `server/internal/ws/event.go` | 5 种事件类型常量 + close code（`4401` / `4400`）定义；事件帧构造函数 `NewResourceEvent` / `NewDeleteEvent` | **不要**超出这 5 种事件之外新增类型；改 close code 需要同步客户端 `ws.ts` / `RealtimeClient.swift` |
| `server/internal/service/broadcaster.go` | `EventBroadcaster` interface（权威入口）+ `nopBroadcaster` 空实现；生产实现在 `ws/adapter.go` 的 `HubBroadcaster` | 加新事件方法时 interface + nop + HubBroadcaster 三处都要加，否则 `var _ EventBroadcaster = nopBroadcaster{}` / `var _ service.EventBroadcaster = (*HubBroadcaster)(nil)` 编译兜底会报错 |

---

## API 约定

路由分三类（见 `router.go`）：

- **公开接口**（无鉴权）：`GET /health`、`POST /api/login`、`GET /app`、`HEAD /app`、`ANY /app/*filepath`（`ANY` 只是把所有方法都转给 webui handler；非 GET/HEAD 在 handler 内部会回 `405 Method Not Allowed` + `Allow: GET, HEAD`，详见 [embed 约定](#embed-约定)）
- **鉴权接口**：挂在 `authed := r.Group("/api"); authed.Use(middleware.Auth(...))` 下的一切，即 `/api/todos/*`、`/api/audit-logs`、`/api/tags`、`/api/sticky-notes/*`；使用 `Authorization: Bearer <jwt>`
- **实时事件通道**：`GET /api/ws`（HTTP Upgrade → WebSocket）。**鉴权不走 `Authorization` header**——浏览器 `WebSocket` 构造器不支持自定义 header，改为"首帧 auth" 协议（见下方说明）。路由注册在公开路由段，握手后由 `ws.Handler` 自行校验 token，**不经过 `middleware.Auth`**。
- **404 / 405 兜底**：由 `r.NoRoute` / `r.NoMethod` 统一回 JSON

### WebSocket 协议契约（`/api/ws`）

完整实现见 `server/internal/ws/`。

**协议帧**：

| 方向 | 帧内容 | 时机 |
|---|---|---|
| C → S | `{"type":"auth","token":"<jwt>"}` | 握手完成后**必须 2 秒内**发送；否则服务端以 close code `4401` 断开 |
| S → C | `{"type":"ready","server_time":"<RFC3339>"}` | auth 成功后服务端推送的第一帧，客户端收到后才算"可以开始消费业务事件" |
| S → C | `{"type":"<event>","data":<资源 JSON>}` 或 `{"type":"<event>","id":<主键>}` | REST 写操作成功后广播 |

**事件类型**（定义在 `server/internal/ws/event.go`，共 **5** 种，**不要**在此之外新增）：

| type | 何时触发 | 载荷 |
|---|---|---|
| `todo.created` | `POST /api/todos` 成功 | `data`：完整 Todo JSON |
| `todo.updated` | `PUT /api/todos/:id` / `complete` / `reopen` / `restore` 成功 | `data`：完整 Todo JSON（restore 也走这条，不是 `todo.restored`） |
| `todo.deleted` | `DELETE /api/todos/:id` 成功 | `id`：Todo 主键（uint） |
| `sticky.upserted` | `PUT /api/sticky-notes/:id` 成功 | `data`：完整 StickyNote JSON |
| `sticky.deleted` | `DELETE /api/sticky-notes/:id` 成功 | `id`：StickyNote 主键（string） |

**Close code**（服务端主动断开时使用；应用自定义区间 4000-4999）：

| code | 语义 | 客户端应对 |
|---|---|---|
| `4401` | auth 超时 / token 非法 / token 过期 | **不重连**，必须清 token 走登出流程 |
| `4400` | 客户端发了非法上行业务消息（`/api/ws` 除首帧外不接受任何上行业务帧） | 视作协议违规，可等用户下次操作再重连 |

**其它注意事项**：

- **心跳**：服务端每 `pingPeriod = 30s` 主动发 WebSocket ping（`client.go`），客户端必须回 pong；服务端 `SetPongHandler` 在收到 pong 后把读超时重置 `pongWait = 60s`（注意：服务端**没有**注册 `SetPingHandler`，gorilla/websocket 默认 handler 只自动回 pong、**不重置 readDeadline**——所以让连接保活的唯一路径是"服务端 ping → 客户端 pong"，客户端单向发 ping 无法延续读超时）。60s 内没拿到任何 pong 就会触发 `SetReadDeadline` 过期 → 读循环 EOF → Hub 移除客户端。
  - 浏览器 `WebSocket` API 自动响应 ping，无需应用代码处理
  - macOS `URLSessionWebSocketTask` 自动处理服务端 ping 帧，但 `RealtimeClient` 额外跑 15s 的客户端侧 `sendPing`——用于**探测本端到服务端的链路是否仍活着**（弱网下 receive 可能长时间 hang 而不抛错），一旦发送失败就能通过 completion / 下一次 receive 报错尽快触发重连
  - Windows `core::WebSocketClient`（基于 `winhttp.dll` 的 `WinHttpWebSocket*` API）同样采用**被动响应 ping + 主动发 ping 兜底**的双保险——本端每 15s 调 `WinHttpWebSocketSend` 发一帧 ping，让 `WinHttpWebSocketReceive` 在网络黑洞时尽快拿到 ERROR 而不是无限 hang
- **CheckOrigin**：`ws.Handler` 的 `makeOriginChecker` 比 REST 的 CORS 多一条"同源放行"——因为浏览器对 WS 握手仍会带 Origin（RFC 6455 §10.2），而 router 的 CORS 中间件依赖浏览器同源时省略 Origin
- **Hub 不缓冲历史事件**：客户端**必须**在 reconnected 时自行全量 refetch（Web 端 `useRealtimeSync` 的 `'reconnected'` signal、macOS 端 `RealtimeClient` 的 `.reconnected` signal、Windows 端 `AppState::OnWebSocketReconnected()` 都会触发这一点）
- **广播对称**：**发起写请求的客户端本身也会收到同一事件**（hub 不做 sender 过滤）。因此各客户端的 mutation 必须用"服务端响应直接写 cache"策略实现本端写入的即时反馈，不能依赖 WS 事件绕一圈回来，否则会与 REST 响应 Promise 产生竞速

### 响应体约定

- 错误一律返回 `{"error": "message"}`；当前 handler / middleware / router 实际使用的状态码集合（`grep -roh 'Status...' server/internal/{handler,middleware,router}/` 结果）：`400 BadRequest` / `401 Unauthorized` / `404 NotFound` / `405 MethodNotAllowed` / `500 InternalServerError`
- 成功响应：`200 OK`（读和大多数写入）、`201 Created`（新建资源，如 `POST /api/todos`）、`204 NoContent`（删除），body 要么是资源对象，要么是分页对象 `{items, total, page, page_size}`
- `/health` 返回 `{"status":"ok","time":"<RFC3339 UTC>","server":"todo-server","version":"<version>"}`（字段顺序以 `router.go` 里 `gin.H{}` 字面量为准；Go map 序列化顺序恰好稳定因为用的是有序 `gin.H`）。`version` 的取值优先级：`Deps.Version`（由 main.go 从 `-ldflags "-X main.version=..."` 注入）→ 为空时回退为字符串字面量 `"unknown"`（**不是** `"dev"`；`dev` 只是 `package-*.sh` 的 `VERSION` 默认值，两者在不同层）

完整接口清单见 [server/README.md](../server/README.md)。

---

## 数据库与迁移

**`data/todo.db`** 是唯一数据源，共四张表（`server/internal/model/models.go`）：

| GORM 模型 | 用途 | 主键策略 |
|---|---|---|
| `Todo` | TODO 业务数据，带软删（`gorm.DeletedAt`）| 自增 `uint` |
| `AuditLog` | 登录 / TODO 变更 / 便签变更审计，`Detail` 是 JSON 字符串 | 自增 `uint` |
| `AppSecret` | K/V 配置，当前只有一行 `key='jwt_secret'` 的 JWT 签名密钥 | `Key string` |
| `StickyNote` | 便签跨端同步用的**服务端模型**（`Frame`/`BgColor`/`Filter` 都是 JSON 字符串整块存）| 客户端生成的 UUID 字符串 |

> **云端数据源重构后的事实（2025）**：`/api/sticky-notes` 是两端客户端的**唯一**便签数据源。
>   - Web 端通过 TanStack Query 拉取，不再做 `zustand/persist`；`stickyStore.ts` 已删除
>   - macOS 端通过 `APIClient.listStickies()` 拉取，`StickyStore.swift` 已删除
>   - **`Frame` 字段在客户端都不再消费**：窗口位置属于本机 UI 偏好，不跨端同步。macOS 端的 `FrameStore` 独立存 `UserDefaults`（key `stickytodo.frames`）；Web 端不维护窗口位置（浏览器里"便签"本质是一张 Card）。两端 `PUT /api/sticky-notes/:id` 请求体里的 `frame` 字段恒为 `"{}"`——保留这个字段是为了服务端 schema 稳定，**不要**因为"客户端不用"就删除它或改为可空

**约定**：

- **没有**独立的 migration 文件，全靠 GORM `AutoMigrate`。**只增字段、不改字段含义**——如果必须重命名或改类型，写 Go 代码做一次性迁移再移除
- 连接 DSN 里挂了三个 pragma：`foreign_keys(1)` / `journal_mode(WAL)` / `busy_timeout(5000)`（见 `db.go`），改驱动或改 DSN 时不要漏

---

## embed 约定

**为什么用 `server/internal/webui/dist/` 而不是仓库根的 `webapp/`**：

- **Go `//go:embed` 指令只能 embed 当前包或子目录**，所以 embed 目录必须和 `webui.go` 在同一个 Go 包下
- `dist/` 里常驻一个 `.gitkeep`（`git ls-files` 能看到），内容文件被 `.gitignore` 忽略，CI 构建时才由 `package-web.sh` 同步进去
- 本地开发时 `go run` 无需先构建 web——`webui.go` 自带 placeholder 页告诉你"请先跑 `scripts/package-web.sh`"

**HTTP 头策略**：

| 请求 | Cache-Control | 说明 |
|---|---|---|
| `/app/` 和 SPA fallback | `no-store` | index.html 每次必须重新拿，否则老 index 指向已删的 hashed chunk 会 404 |
| `/app/assets/*` | `public, max-age=31536000, immutable` | Vite 产物自带 content hash，可长缓存 |

所有 `/app/*` 响应统一带上以下安全头（`webui.go` handler 硬编码，详情见源文件）：

- `X-Content-Type-Options: nosniff`
- `Referrer-Policy: no-referrer`
- `Content-Security-Policy:` 完整值为 `default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' data:; font-src 'self' data:; connect-src 'self'; frame-ancestors 'none'; base-uri 'self'`
  - 允许 inline **style**（Tailwind JIT 产物可能用到）、禁止 inline **script**、`frame-ancestors 'none'` 防点击劫持、`connect-src 'self'` 保证 fetch 只能同源

对非 GET/HEAD 方法，`/app/*` 额外回 `405 Method Not Allowed` + `Allow: GET, HEAD`。
