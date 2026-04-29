# AGENTS.md — stickytodo 项目架构

面向开发者 / AI 代理的**工程指南**。安装和使用请看 [README.md](./README.md)。

本文目标：让任何一个新上手的人（或 agent）在读完本文后，就能独立完成改代码、跑测试、发版本这三件事，而不需要再去翻代码猜架构。

---

## 1. 全局视图

stickytodo 是一个 **C/S 架构** 的单账号 TODO 工具，一个后端配两种客户端：

```
┌──────────────────────┐    HTTPS / HTTP           ┌──────────────────────────┐
│ macOS 菜单栏客户端    │ ─── REST + JWT ───────▶   │                          │
│ (Swift / SwiftUI)    │                           │   stickytodo-server       │
└──────────────────────┘                           │   (Go + Gin + GORM       │
                                                   │    + glebarez/sqlite)     │
┌──────────────────────┐                           │                          │
│ 浏览器 Web UI         │ ─── REST + JWT ───────▶   │   ┌────────────────────┐ │
│ (React + Vite,       │                           │   │ go:embed 内嵌 Web   │ │
│  内嵌到 server 二进制)│ ◀── GET /app/ ───────────│   │ (dist/ → /app/)    │ │
└──────────────────────┘                           │   └────────────────────┘ │
                                                   │                          │
                                                   │   SQLite (单文件)         │
                                                   └──────────────────────────┘
```

核心设计取舍：

- **单二进制分发**：Web UI 通过 `go:embed` 打进后端，部署时不需要 nginx 做静态托管。
- **零 CGO**：SQLite 使用 `github.com/glebarez/sqlite`（基于 `modernc.org/sqlite`，纯 Go），可以 `CGO_ENABLED=0` 交叉编译所有平台，配合 distroless 基础镜像产出极小体积容器。
- **JWT 密钥自管理**：server 首次启动生成 32 字节随机密钥，持久化到 SQLite 的 `app_secrets` 表，**重启不变**，所以用户不需要配环境变量，也不会因重启踢下线。

---

## 2. 仓库目录

```
.
├── README.md                  # 用户文档：只讲安装和使用
├── AGENTS.md                  # 本文件：开发者 / 代理架构文档
├── assets/branding/           # 品牌视觉资产（单一真相源）
│   ├── stickytodo-icon.svg    # 1024×1024 主 SVG 设计稿（**彩色** brand mark）；AppIcon/favicon 派生
│   ├── stickytodo-menubar.svg # 18×18pt **模板图**（纯黑 + alpha），menubar MenuBarIcon 派生
│   └── out/                   # 脚本生成目录（.gitignore 之外的 AppIcon.icns 等）
├── server/                    # Go 后端（单模块 go.mod）
│   ├── cmd/todo-server/       # main 入口（支持 -port / -username / -password / -version flag）
│   ├── internal/
│   │   ├── config/            # 环境变量解析
│   │   ├── model/             # GORM 模型（Todo/AuditLog/AppSecret/StickyNote）+ DB 初始化（driver 在此切换）
│   │   ├── repository/        # 数据访问层（只与 *gorm.DB 打交道）
│   │   ├── service/           # 业务逻辑（鉴权、TODO、审计、便签）；
│   │   │                      # broadcaster.go 定义 EventBroadcaster interface（nopBroadcaster 默认实现 + ws.HubBroadcaster 生产实现）
│   │   ├── handler/           # Gin HTTP handler（薄层 DTO 映射）
│   │   ├── middleware/        # 仅 auth.go（JWT）；CORS 实现在 router.go 本地函数里
│   │   ├── router/            # 路由装配，/app 挂载 webui.Handler
│   │   ├── webui/             # go:embed dist/ → http.Handler
│   │   └── ws/                # WebSocket 实时事件广播：event.go / hub.go / client.go / handler.go / adapter.go
│   ├── scripts/
│   │   ├── smoke.sh           # 36 步端到端冒烟脚本（Step 33-36 是 WS 回归）
│   │   └── ws-probe/main.go   # smoke.sh 启动时 go build 出来的临时 WS 探针二进制
│   ├── Dockerfile             # 多阶段构建：web → go → distroless
│   ├── docker-compose.yml
│   └── .env.example
├── client/
│   ├── mac/                   # macOS SwiftUI 客户端（stickytodo.xcodeproj）
│   │   └── stickytodo/
│   │       ├── StickyTodoApp.swift  # @main；同文件内定义 StickyWindowBridge（Combine sink 订阅 AppState）
│   │       ├── AppState.swift       # @MainActor 全局状态：认证 + 云端 stickies + APIClient + RealtimeClient + FrameStore；WS 事件路由
│   │       ├── Models/              # DTO（Todo / AuditLog / StickyNote / Filter；与后端 JSON 对齐）
│   │       ├── Networking/          # APIClient（REST）+ Endpoints + RealtimeClient（/api/ws）
│   │       ├── Storage/             # KeychainStore（JWT）+ FrameStore（窗口位置 UserDefaults key "stickytodo.frames"）
│   │       ├── Windows/             # StickyWindowController + StickyWindowManager（每便签一 NSWindow）
│   │       └── Views/               # 9 个文件：MenuBarContent / SettingsView（3 Tab）/ StickyView / StickyViewModel /
│   │                                # TodoRow / DraftTodoRow / FilterEditor / HistoryView / WindowDragHandle
│   ├── web/                   # React + Vite + Tailwind + Zustand + TanStack Query
│   │   ├── src/
│   │   │   ├── api/           # client.ts（fetch 封装）+ queryKeys.ts + ws.ts（stickyWS 单例，首帧 auth 协议）
│   │   │   ├── hooks/         # useRealtimeSync.ts（桥接 stickyWS → TanStack Query invalidate）
│   │   │   ├── store/         # authStore + uiStore（仅两个；stickyStore 已在云端数据源重构中删除）
│   │   │   ├── types/         # 与后端对齐的 TypeScript DTO（含 sticky.ts）
│   │   │   ├── lib/           # color / format / stickyCodec（StickyNoteDTO ↔ StickyView）
│   │   │   ├── components/    # AppBar / StickyCard / TodoList / HistoryView / ...
│   │   │   └── views/         # StickyBoard + LoginView
│   │   └── vite.config.ts     # base='/app/', dev proxy → 127.0.0.1:8080（HTTP-only，不代理 WS）
│   └── scripts/build.sh       # macOS 客户端本地回归
├── scripts/                   # 打包脚本（本地可单独跑，CI 也复用）
│   ├── package-web.sh         # 构建 web + 同步到 server/internal/webui/dist
│   ├── package-server.sh      # 7 份跨平台二进制 + SHA256SUMS
│   ├── package-mac-client.sh  # xcodebuild universal → codesign → DMG
│   ├── package-docker.sh      # 单架构 docker build（多架构交给 CI）
│   └── generate-icons.sh      # 从 assets/branding/*.svg 派生：Mac AppIcon.appiconset + MenuBarIcon.imageset（template）+ AppIcon.icns + Web favicons
├── .github/workflows/
│   ├── _build-all.yml         # 可复用 workflow，6 job：build-web / build-server(matrix) / build-mac-dmg / detect-docker-creds / build-docker(qemu+buildx) / publish-release
│   ├── release-tag.yml        # push tag v* 自动走正式发布
│   └── release-branch.yml     # workflow_dispatch 分支预发布（先删旧 release）
└── docs/RELEASE.md            # 发版操作手册
```

---

## 3. 后端架构（server/）

### 3.1 分层

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

### 3.2 关键文件速查

| 文件 | 作用 | 改它时要注意 |
|---|---|---|
| `server/internal/config/config.go` | 所有 `TODO_*` 环境变量解析 + 校验（Port / Username / Password / DataDir / TokenTTL / GinMode / Verbose） | 加新变量时：①新增字段 + getter 默认值 ②在 `Validate` 里校验 ③同步 `.env.example` 和根 README 的配置表 |
| `server/cmd/todo-server/main.go` | 进程入口；读 `TODO_CORS_ORIGINS` 填 router `Deps.CorsOrigins`；带 `-version` 子命令；支持 `-port` / `-username` / `-password` flag 覆盖同名环境变量（flag 非空时优先） | `TODO_CORS_ORIGINS` 不在 `config.go` 而在这里 `parseCorsOrigins`；三个 override flag 通过 `applyFlagOverride` 用 `os.Setenv` 注入，之后仍走 `config.Load` 统一校验，避免校验分叉 |
| `server/internal/model/db.go` | SQLite 驱动选择、DSN 拼装（`_pragma=...`）、AutoMigrate | 改驱动会影响 Dockerfile / CI 是否需要 CGO |
| `server/internal/model/models.go` | 所有 GORM 模型 + JSON tag | 字段改动必须同步 `client/web/src/types/api.ts` 和 Swift 的 `Models/` |
| `server/internal/service/auth_service.go` | JWT 签发 + 校验；首次启动生成 32 字节熵、hex 编码落 `app_secrets` | 改签名算法或 claim 结构 = 强制所有存量 token 失效 |
| `server/internal/middleware/auth.go` | 仅一个 `Auth(*service.AuthService)`，解析 Bearer header、注入 `actor` 到 gin.Context | 校验失败统一 401 `{"error": ...}`；别在这里加其他业务逻辑 |
| `server/internal/webui/webui.go` | `//go:embed all:dist` + SPA fallback + CSP | 修改前读 [§3.5 embed 约定](#35-embed-约定) |
| `server/internal/router/router.go` | 路由注册、`corsMiddleware` 本地函数、`/app` 的 GET/HEAD 301 | `/app → /app/` 必须 GET 和 HEAD 都注册；`Deps.CorsOrigins` 为空时不注入 CORS |
| `server/scripts/smoke.sh` | 36 步端到端冒烟（HTTP 黑盒 + Step 33-36 WS 回归），本项目**唯一**回归工具；启动时会 `go build ./scripts/ws-probe` 到 mktemp 作为 WS 探针 | 新增 API 或修改既有契约时必须同步加步骤，否则 CI 发不出来也发现不了；新增 WS 事件类型必须在 Step 33-36 附近加 ws-probe 校验 |
| `server/scripts/ws-probe/main.go` | `smoke.sh` 默认 `go build -o $(mktemp -d)/ws-probe ./scripts/ws-probe` 构建的 WebSocket 探针二进制；可通过导出 `WS_PROBE_BIN=/path/to/prebuilt` 环境变量复用预编译产物跳过构建（见 `smoke.sh:30` 分支）。4 种模式 `no-auth` / `bad-token` / `auth-ready` / `wait-event` 分别对应 Step 33/34/35/36；退出码 `0=pass / 1=assertion fail / 2=usage error` | 改 WS 协议帧格式（auth / ready / 事件帧）时同步改 ws-probe 的解析逻辑，否则 smoke.sh 假阳性 |
| `server/internal/ws/event.go` | 5 种事件类型常量 + close code（`4401` / `4400`）定义；事件帧构造函数 `NewResourceEvent` / `NewDeleteEvent` | **不要**超出这 5 种事件之外新增类型；改 close code 需要同步客户端 `ws.ts` / `RealtimeClient.swift` |
| `server/internal/service/broadcaster.go` | `EventBroadcaster` interface（权威入口）+ `nopBroadcaster` 空实现；生产实现在 `ws/adapter.go` 的 `HubBroadcaster` | 加新事件方法时 interface + nop + HubBroadcaster 三处都要加，否则 `var _ EventBroadcaster = nopBroadcaster{}` / `var _ service.EventBroadcaster = (*HubBroadcaster)(nil)` 编译兜底会报错 |

### 3.3 API 约定

路由分三类（见 `router.go`）：

- **公开接口**（无鉴权）：`GET /health`、`POST /api/login`、`GET /app`、`HEAD /app`、`ANY /app/*filepath`（`ANY` 只是把所有方法都转给 webui handler；非 GET/HEAD 在 handler 内部会回 `405 Method Not Allowed` + `Allow: GET, HEAD`，详见 §3.5）
- **鉴权接口**：挂在 `authed := r.Group("/api"); authed.Use(middleware.Auth(...))` 下的一切，即 `/api/todos/*`、`/api/audit-logs`、`/api/tags`、`/api/sticky-notes/*`；使用 `Authorization: Bearer <jwt>`
- **实时事件通道**：`GET /api/ws`（HTTP Upgrade → WebSocket）。**鉴权不走 `Authorization` header**——浏览器 `WebSocket` 构造器不支持自定义 header，改为"首帧 auth" 协议（见下方说明）。路由注册在公开路由段，握手后由 `ws.Handler` 自行校验 token，**不经过 `middleware.Auth`**。
- **404 / 405 兜底**：由 `r.NoRoute` / `r.NoMethod` 统一回 JSON

WebSocket 协议（`/api/ws`）契约摘要（完整实现见 `server/internal/ws/`）：

| 方向 | 帧内容 | 时机 |
|---|---|---|
| C → S | `{"type":"auth","token":"<jwt>"}` | 握手完成后**必须 2 秒内**发送；否则服务端以 close code `4401` 断开 |
| S → C | `{"type":"ready","server_time":"<RFC3339>"}` | auth 成功后服务端推送的第一帧，客户端收到后才算"可以开始消费业务事件" |
| S → C | `{"type":"<event>","data":<资源 JSON>}` 或 `{"type":"<event>","id":<主键>}` | REST 写操作成功后广播 |

事件类型（定义在 `server/internal/ws/event.go`，共 **5** 种，**不要**在此之外新增）：

| type | 何时触发 | 载荷 |
|---|---|---|
| `todo.created` | `POST /api/todos` 成功 | `data`：完整 Todo JSON |
| `todo.updated` | `PUT /api/todos/:id` / `complete` / `reopen` / `restore` 成功 | `data`：完整 Todo JSON（restore 也走这条，不是 `todo.restored`） |
| `todo.deleted` | `DELETE /api/todos/:id` 成功 | `id`：Todo 主键（uint） |
| `sticky.upserted` | `PUT /api/sticky-notes/:id` 成功 | `data`：完整 StickyNote JSON |
| `sticky.deleted` | `DELETE /api/sticky-notes/:id` 成功 | `id`：StickyNote 主键（string） |

Close code（服务端主动断开时使用；应用自定义区间 4000-4999）：

| code | 语义 | 客户端应对 |
|---|---|---|
| `4401` | auth 超时 / token 非法 / token 过期 | **不重连**，必须清 token 走登出流程 |
| `4400` | 客户端发了非法上行业务消息（`/api/ws` 除首帧外不接受任何上行业务帧） | 视作协议违规，可等用户下次操作再重连 |

其它注意事项：

- 心跳：服务端每 `pingPeriod = 30s` 主动发 WebSocket ping（`client.go`），客户端必须回 pong；服务端 `SetPongHandler` 在收到 pong 后把读超时重置 `pongWait = 60s`（注意：服务端**没有**注册 `SetPingHandler`，gorilla/websocket 默认 handler 只自动回 pong、**不重置 readDeadline**——所以让连接保活的唯一路径是"服务端 ping → 客户端 pong"，客户端单向发 ping 无法延续读超时）。60s 内没拿到任何 pong 就会触发 `SetReadDeadline` 过期 → 读循环 EOF → Hub 移除客户端。浏览器 `WebSocket` API 会自动响应 ping，无需应用代码处理；`URLSessionWebSocketTask`（macOS）也会自动处理服务端 ping 帧，但 `RealtimeClient` 额外跑了一个 15s 的客户端侧 `sendPing`——它的作用是**探测本端到服务端的链路是否仍活着**（弱网下 receive 可能长时间 hang 而不抛错），一旦发送失败就能通过 completion / 下一次 receive 报错尽快触发重连
- CheckOrigin：`ws.Handler` 的 `makeOriginChecker` 比 REST 的 CORS 多一条"同源放行"——因为浏览器对 WS 握手仍会带 Origin（RFC 6455 §10.2），而 router 的 CORS 中间件依赖浏览器同源时省略 Origin
- Hub 不缓冲历史事件：客户端**必须**在 reconnected 时自行全量 refetch（Web 端 `useRealtimeSync` 的 `'reconnected'` signal、macOS 端 `RealtimeClient` 的 `.reconnected` signal 都会触发这一点）
- 广播对称：**发起写请求的客户端本身也会收到同一事件**（hub 不做 sender 过滤）。因此各客户端的 mutation 必须用"服务端响应直接写 cache"策略实现本端写入的即时反馈，不能依赖 WS 事件绕一圈回来，否则会与 REST 响应 Promise 产生竞速

响应体约定：

- 错误一律返回 `{"error": "message"}`；当前 handler / middleware / router 实际使用的状态码集合（`grep -roh 'Status...' server/internal/{handler,middleware,router}/` 结果）：`400 BadRequest` / `401 Unauthorized` / `404 NotFound` / `405 MethodNotAllowed` / `500 InternalServerError`
- 成功响应：`200 OK`（读和大多数写入）、`201 Created`（新建资源，如 `POST /api/todos`）、`204 NoContent`（删除），body 要么是资源对象，要么是分页对象 `{items, total, page, page_size}`
- `/health` 返回 `{"status":"ok","time":"<RFC3339 UTC>","server":"todo-server","version":"<version>"}`（字段顺序以 `router.go` 里 `gin.H{}` 字面量为准；Go map 序列化顺序恰好稳定因为用的是有序 `gin.H`）。`version` 的取值优先级：`Deps.Version`（由 main.go 从 `-ldflags "-X main.version=..."` 注入）→ 为空时回退为字符串字面量 `"unknown"`（**不是** `"dev"`；`dev` 只是 `package-*.sh` 的 `VERSION` 默认值，两者在不同层）

完整接口清单见 [server/README.md](./server/README.md)。

### 3.4 数据库与迁移

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

约定：

- **没有**独立的 migration 文件，全靠 GORM `AutoMigrate`。**只增字段、不改字段含义**——如果必须重命名或改类型，写 Go 代码做一次性迁移再移除
- 连接 DSN 里挂了三个 pragma：`foreign_keys(1)` / `journal_mode(WAL)` / `busy_timeout(5000)`（见 `db.go`），改驱动或改 DSN 时不要漏

### 3.5 embed 约定

为什么用 `server/internal/webui/dist/` 而不是仓库根的 `webapp/`：

- **Go `//go:embed` 指令只能 embed 当前包或子目录**，所以 embed 目录必须和 `webui.go` 在同一个 Go 包下
- `dist/` 里常驻一个 `.gitkeep`（`git ls-files` 能看到），内容文件被 `.gitignore` 忽略，CI 构建时才由 `package-web.sh` 同步进去
- 本地开发时 `go run` 无需先构建 web——`webui.go` 自带 placeholder 页告诉你"请先跑 `scripts/package-web.sh`"

HTTP 头策略：

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

---

## 4. 前端架构

### 4.1 Web（client/web/）

技术栈：**React 18 + Vite 5 + TypeScript + Tailwind 3 + Zustand 4 + TanStack Query v5 + date-fns 3 + lucide-react**（版本见 `client/web/package.json`）

核心抽象：

- **API 层 (`src/api/`)**：
  - `client.ts`：`fetch` 薄封装，统一注入 `Authorization: Bearer`、401 自动 logout、`ApiError` 结构化错误。所有请求经由单个 `request<T>()` 函数，路径前缀是**空字符串**（生产同源 `/api`，开发 Vite proxy 转发到 8080）。新增的 sticky 方法（`listStickies` / `upsertSticky` / `deleteSticky` / `getSticky`）在 `client.ts` 内部通过 `hexToBgColorJSON` / `filterToJSON`（见 `src/lib/stickyCodec.ts`）完成"hex 颜色 ↔ CodableRGBA JSON"和"TodoFilter 对象 ↔ 字符串"的双向编解码，上层组件只需消费 `StickyView`
  - `queryKeys.ts`：TanStack Query cache key 集中管理，包含 `qk.stickies()`
  - `ws.ts`：原生 `WebSocket` 单例客户端 `stickyWS`。实现了首帧 auth、指数退避 `[1,2,4,8,16,30]s`、`visibilitychange` 立即重连、`close code 4401 → 'unauthorized' signal` 等完整协议逻辑；业务层通过 `onEvent` / `onSignal` 订阅
- **状态层 (`src/store/`)**，实际只有两个 store（`stickyStore.ts` 已在云端数据源重构中删除，便签数据改由 TanStack Query 管理）：
  - `authStore.ts`：JWT token + username，通过 `zustand/persist` 存 `localStorage`
  - `uiStore.ts`：深色模式偏好，`type DarkMode = 'system' | 'light' | 'dark'`（注意 system 排第一个，枚举真值以代码为准）
- **Hook 层 (`src/hooks/`)**：
  - `useRealtimeSync.ts`：桥接 `stickyWS` 与 TanStack Query cache。监听 `authStore.token` 变化控制连接；收到 `todo.*` 事件 → `queryClient.invalidateQueries({queryKey: qk.todos(...)})`；收到 `sticky.*` 事件 → invalidate `qk.stickies()`；收到 `'reconnected'` signal → 全量 invalidate；收到 `'unauthorized'` → 调用 `authStore.logout()`
- **工具层 (`src/lib/`)**：
  - `color.ts`：`hexToRgb` / `luminance` / `isLightBackground` / `priorityColor` / `foregroundFor`，全部入参 `string | null | undefined` 容错
  - `format.ts`：`formatDue` / `formatRelative` / `filterSummary` / `toISOFromLocalInput` / `toLocalInputFromISO`，依赖 date-fns
  - `stickyCodec.ts`：`StickyNoteDTO ↔ StickyView` 双向转换；解码侧所有异常都用 `DEFAULT_STICKY_COLOR` / `defaultFilter` 兜底（脏数据不阻塞 UI），编码侧保证输出合法 JSON（后端 `json.Valid` 校验必过）；`viewToUpsertRequest` 里 `frame` 恒 `"{}"`
- **服务端状态 (`TanStack Query`)**：所有远端数据都经 `useQuery` / `useMutation`，cache key 由 `queryKeys.ts` 集中管理；便签列表也由 `useQuery(qk.stickies(), api.listStickies)` 订阅，与 TODO 数据走同一条缓存链路
- **视图**：`views/StickyBoard` → `components/StickyCard` → `components/TodoList` → `components/TodoRow`，一张便签就是一个过滤器，多张便签可以订阅不同筛选条件并排放；`AppBar` 提供全局历史入口 + "新建便签"（通过 `useMutation(api.upsertSticky)` 乐观更新）；`HistoryView` 是审计日志弹窗、`EditTodoSheet` / `FilterEditor` / `DraftTodoRow` / `Modal` 是配套交互组件。`App.tsx` 在根部挂载 `useRealtimeSync()` hook 统一驱动 WS

Vite 关键配置：`base: '/app/'`（和后端 embed 挂载路径一致）、`build.outDir: 'dist'`、dev 时 proxy `/api`、`/health` 到 `127.0.0.1:8080`。

**dev 模式下 WebSocket 的已知约束**：`stickyWS` 用 `window.location.origin` 翻译成 `ws(s)://host/api/ws`——生产环境同源（`/app/` 和 `/api/ws` 共享 `window.location.host`）正确；但 `vite.config.ts` 的 `server.proxy['/api']` 目前**没有显式配 `ws: true`**，`http-proxy-middleware` 在缺省 `ws` 选项时**不会**代理 WebSocket 升级请求。所以开发时前端跑在 5173、后端跑在 8080 的情况下，WS 握手会被 Vite dev server 以 404 拒绝。临时绕过办法：在本地给 `ws.ts` 的 `connect(token, baseURL)` 第二参数显式传 `'http://127.0.0.1:8080'`，让 WS 直连后端；长期方案是给 vite.config 的 `/api` proxy 加 `ws: true`（此改动不在本重构范围内，按需独立提 PR）。

### 4.2 macOS 客户端（client/mac/）

技术栈：**Swift 5.9 + SwiftUI + Combine + Keychain Services**。`@Published` 在多数视图里以 SwiftUI 的 `@EnvironmentObject` / `@ObservedObject` 方式消费；只有 `StickyWindowBridge` 显式 `import Combine` 用 `sink` + `AnyCancellable` 直接订阅——因为它不是 View，挂在 SwiftUI 生命周期里会在 MenuBarExtra 面板折叠时失去响应

Bundle 和命名（真值来自 `stickytodo.xcodeproj/project.pbxproj`）：

- Bundle ID：`com.hanxi.stickytodo`（`PRODUCT_BUNDLE_IDENTIFIER`）
- 部署目标：macOS 13.0+（`MACOSX_DEPLOYMENT_TARGET = 13.0`）
- 版本号：`MARKETING_VERSION = 1.0`（当前硬编码在 pbxproj，`package-mac-client.sh` **不修改** Info.plist，只把 `$VERSION` 打进产物文件名）
- Keychain service name：`com.hanxi.stickytodo`（`KeychainStore.service`，存储 JWT）
- UserDefaults：`UserDefaults.standard`（**非** App Group suite），本机窗口位置持久化 key 是 `stickytodo.frames`（`FrameStore.defaultsKey`）——便签业务数据本身已改走服务端，不再落 UserDefaults
- `os.Logger` subsystem：`com.hanxi.stickytodo`
- 菜单栏图标：Assets.xcassets 中的 **`MenuBarIcon`**（template image，由 `scripts/generate-icons.sh` 从 `assets/branding/stickytodo-menubar.svg` 渲染产出）；**无 Dock 图标**（Info.plist `LSUIElement=YES`）
- **模板图（template image）规则**：`MenuBarIcon.imageset/Contents.json` 里必须有 `"properties":{"template-rendering-intent":"template"}`（注意 `properties` 是顶层字段，不是塞在每张图里）。这样系统才会自动按"明/暗菜单栏 + 选中态"反色。源 SVG 只能用纯黑 `#000000` + alpha；**不要**在 SVG 里画彩色——主 brand mark 的黄/绿配色属于 AppIcon，不是 menubar
- App 图标（Dock / Finder / About / DMG）：Assets.xcassets 中的 **`AppIcon`**（`ASSETCATALOG_COMPILER_APPICON_NAME = AppIcon`）；SwiftUI 侧在 `StickyTodoApp.swift` 用 `Image("MenuBarIcon")`（注意是名称加载，不是 `systemName:`；写成 `Image(systemName: "MenuBarIcon")` 会被当作 SF Symbol 查不到而显示占位）

模块职责：

- **AppState**：`@MainActor` + `ObservableObject`，持有 `APIClient`、登录态、云端便签列表、`FrameStore`、`RealtimeClient`。关键职责分两层：
  - **认证 / 数据**：`login(...)` 成功后立即 `bootstrapAfterAuth()` → 并行全量拉 `listStickies()` + 启动 `RealtimeClient.connect()`；`logout()` 同步 `RealtimeClient.disconnect()` + 清 stickies
  - **WS 事件路由**：`sticky.upserted` / `sticky.deleted` 直接 merge 到 `@Published var stickies: [StickyNote]`；`todo.*` 通过 `NotificationCenter.default.post(name: .stickyTodoCreated/Updated/Deleted, userInfo: [AppStateNotification.todoKey: event])` 广播给各个 `StickyViewModel`——ViewModel 不直接耦合 AppState，解耦点就在这组 `Notification.Name` 常量上
- **Networking/APIClient**：纯 `URLSession`（async/await），方法签名与后端 REST 一一对应，失败时抛结构化 `APIError`。sticky 方法（`listStickies` / `getSticky` / `upsertSticky` / `deleteSticky`）内部通过 `encodeBgColor` / `encodeFilter` 完成"CodableRGBA ↔ JSON 字符串"和"TodoFilter ↔ snake_case JSON 字符串"双向转换；TodoFilterDTO 是私有的 snake_case 适配层（Web 端 `filterToJSON` 直接 `JSON.stringify(filter)` 产出 snake_case，macOS `TodoFilter.CodingKeys` 是 camelCase，两端互通必须走这一层）
- **Networking/RealtimeClient**：`URLSessionWebSocketTask` 实现的 WS 客户端，与 Web 端 `stickyWS` 协议行为等价（首帧 auth / 2s 超时 / ready 帧 / 指数退避 `[1,2,4,8,16,30]s` / close code 4401 → `.unauthorized` signal）。额外的"客户端侧主动 ping"（15s）用于在后端服务器 30s ping 之外做保活兜底——`URLSessionWebSocketTask` 不会自动响应服务端 ping，必须有本端 ping 才能维持连接活性。事件用 `RealtimeEvent` struct 透传（`data: Data?` 是原始 JSON bytes，由订阅者按需解码；`id: String?` 统一把 uint 和 string 两种主键转为字符串表示）
- **Storage/KeychainStore**：JWT 读写。Accessible 级别 `kSecAttrAccessibleAfterFirstUnlock`——首次解锁后就能访问，适合菜单栏常驻型 App
- **Storage/FrameStore**：便签窗口位置的**纯本机**持久化。key `stickytodo.frames`，value 是 `[String: CodableRect]` 的 JSON。`StickyNote` 已经不再携带 frame 字段（属于"本机 UI 偏好，不跨设备同步"）；`StickyWindowController` 的 `didMove/didResize` 回调写入这里，`StickyWindowManager.sync` 开新窗口时从这里查（未命中则用 `defaultFrame + 偏移`兜底）
- **Windows/**：**仅两个文件**（`StickyWindowController.swift` + `StickyWindowManager.swift`）——`StickyWindowController` 负责**单个**便签窗口（`window.level = .floating` 实现桌面置顶、`init(note:initialFrame:contentBuilder:)` 签名——frame 由 Manager 从 FrameStore 查出后注入，不从 note 读；把 SwiftUI `StickyView` 注入 `NSHostingView`）；`StickyWindowManager` 负责**多个**窗口集合，`init(frameStore:contentBuilder:)` 注入 FrameStore，按 sticky id 建立窗口，新增/关闭便签时增删对应 `NSWindow`。⚠️ `StickyWindowBridge` **不在** Windows/ 目录下，而是定义在 `StickyTodoApp.swift` 同文件内（`final class StickyWindowBridge: ObservableObject`），作为 App 与 WindowManager 的响应式桥梁；Bridge 的三个回调（`onNewSticky` / `onCloseSticky` / `onNoteChange`）都把 `appState.addSticky/removeSticky/updateSticky` 的 async API 包成 `Task { @MainActor do-catch }` 调用。**订阅机制**：`attach(appState:)` 通过 `import Combine` 的 `appState.$stickies.sink(...)` / `appState.$isAuthenticated.sink(...)` 订阅两个 `@Published` 源，cancellable 持有在 Bridge 自身——**不能**改用 SwiftUI 的 `.onChange` 挂在 `MenuBarExtra { } ` 内部，因为 MenuBarExtra 面板未展开时子树未挂载，`.onChange` 不求值，会导致便签被另一端通过 WS 删除/新增后本机窗口不同步，直到用户点开菜单栏才 catch up（历史 bug）
- **Views/**（共 9 个文件）：
  - `MenuBarContent.swift`：菜单栏点出的主面板。**当前布局三段**：①`headerRow`——品牌标题 + 已登录时在尾部展示用户名；②中段 `authenticatedBody` / `unauthenticatedBody`——已登录时只有**一个**「新建便签」按钮（独占一行、全宽、**`.bordered` 样式**，绑定 `⌘N`。从 `.borderedProminent` 改 `.bordered` 的原因代码注释已写：prominent 按下会切成高亮填充 + 白色前景，深浅色交叉下观感失衡）；未登录时展示一段"尚未登录。请在『设置』中配置服务器地址并登录。"提示 + 一个 **`.borderedProminent`** 样式的「打开设置」按钮；③`footerRow`——无论是否登录都挂在底部：`[设置] [登出（仅已登录）] [退出 ⌘Q]`，其中「退出」是 `.destructive`。**历史入口已整体迁移到 `SettingsView` 的「历史」Tab，MenuBarContent 里不再有「历史」按钮**（文件头注释明确写着"历史查看器已迁移到 Settings → 历史 Tab"）。新建便签的失败路径仅 `print("[MenuBarContent] addSticky failed: ...")`，不弹 alert
  - `StickyView.swift`：单个便签的 SwiftUI 根视图；用 `@StateObject private var viewModel: StickyViewModel` 持有业务逻辑。`onCloseSticky` / `onNoteChange` 回调签名里的 sticky id 类型是 `String`（不是 UUID）。错误呈现靠 `.alert(item: $viewModel.currentError)`，由 ViewModel 的 `@Published var currentError: StickyViewError?` 驱动
  - `StickyViewModel.swift`：`final class StickyViewModel: ObservableObject`，承载单个便签的 TODO 列表、加载状态、错误态（`StickyViewError: Identifiable, Equatable`）等 `@Published` 字段；由 `StickyView` own 其生命周期。**在 `init` 里订阅 4 个 NotificationCenter 事件**（`.stickyTodoCreated/Updated/Deleted/.stickyRealtimeReconnected`），任一事件到来都触发 `scheduleDebouncedRefresh`（300ms 窗口合并多事件为一次 `refresh()`）。observer tokens 用 `nonisolated(unsafe)` 存储以便 `deinit` 能 remove
  - `SettingsView.swift`：`⌘,` 打开的 Settings Scene，**标准 macOS Preferences 风格的 `TabView`**，固定尺寸 `520×420`（在 `body` 上 `.frame(width: 520, height: 420)`），共 **3 个 Tab**：
      - 「设置」（`generalTab`）：服务器 Base URL 表单（`urlDraft` + 合法性状态 `URLValidationState`，「保存地址」会把 `http://` 自动前缀补全 + 「测试连接」→ `GET /health` + 绿色/红色结果文案）+ 账号表单（未登录→用户名/密码登录；已登录→展示账号 + 登出）
      - 「历史」（`historyTab`）：已登录时嵌入 `HistoryView(mode: .global, apiClient: appState.apiClient, embedded: true)`（`embedded: true` 会让 HistoryView 不渲染自己的「关闭」按钮，由外层 Settings 窗口统一关闭）；**未登录时**展示锁图标 + 文案「请先在『设置』Tab 登录后查看历史」的占位视图
      - 「关于」（`aboutTab`）：`Form + formStyle(.grouped)` 风格，内嵌 `aboutBlock`，展示品牌信息、版本号（来自 Info.plist 的 `CFBundleShortVersionString` / `CFBundleVersion`）、Bundle ID、项目链接
  - `TodoRow.swift` / `DraftTodoRow.swift`：已存 TODO / 新建草稿 TODO 的行组件
  - `FilterEditor.swift`：便签绑定的筛选条件编辑器
  - `HistoryView.swift`：变更历史 / 审计日志视图。**两种展示模式**由 `Mode` enum 区分（`.todo(id:title:)` / `.global`）；另有一个 `embedded: Bool = false` 开关——`false`（默认）以独立 `.sheet` 形式弹出、顶部渲染「关闭」按钮（依赖 `@Environment(\.dismiss)`）；`true`（嵌入 Settings TabView）时顶部不渲染关闭按钮，由外层 Settings 窗口统一关闭
  - `WindowDragHandle.swift`：便签顶部不可见的拖动区（便签窗口 `styleMask = [.borderless, .resizable, .fullSizeContentView]`，**无系统标题栏**，靠这里拖动）
- **Models/**（共 4 个，与后端 `models.go` + `types/api.ts` 一一对应）：`Todo.swift` / `AuditLog.swift` / `StickyNote.swift` / `Filter.swift`。`StickyNote.id: String`（客户端生成 UUID 字符串，由 `StickyNote.newID()` 产出）；**不包含 frame 字段**
- **StickyTodoApp.swift**：`@main` 入口，持有**两个** `@StateObject`：
  - `appState: AppState`：纯数据/业务状态
  - `windowBridge: StickyWindowBridge`：App 初始化时立刻把 `AppState.stickies` / `isAuthenticated` 的变化同步到 `StickyWindowManager`——之所以必须在 `App.init()` 就建好，是因为 `MenuBarExtra` 的 `.onAppear` 只有用户点开菜单栏面板才触发，太晚
  - body 只有两条 Scene：`MenuBarExtra { MenuBarContent() } label: { Image(systemName: "note.text") }.menuBarExtraStyle(.window)` 和 `Settings { SettingsView() }`；`Image(systemName:)` 走 SF Symbols，缺了 `systemName:` 会去 Asset Catalog 找同名图片。`.menuBarExtraStyle(.window)` 决定了点菜单栏图标弹出的是一个**浮窗**而非系统菜单
  - ⚠️ **这里绝对不能把"`appState.stickies` / `isAuthenticated` 变化 → 调 `windowBridge.syncWindows`"写成 SwiftUI 的 `.onChange` 挂在 `MenuBarExtra { }` 内部**——菜单栏面板未展开时整个子树不挂载、`.onChange` 不求值，会导致 WS 推送的 sticky 增删不能实时驱动桌面便签窗口更新。Bridge 用 Combine sink 自主订阅即可，StickyTodoApp.body 里**不需要**任何 onChange

快捷键：

- `⌘,` 打开设置：SwiftUI `Settings` Scene 自带的系统级快捷键，App 激活时即可命中，**不依赖菜单栏面板是否展开**；代码里没有也不需要手动 `.keyboardShortcut(",")`。macOS 14+ 走 `SettingsLink`，13 回退到 `NSApp.sendAction(#selector(showSettingsWindow:))`
- `⌘N` 新建便签（`MenuBarContent.swift` 显式绑定 `.keyboardShortcut("n", modifiers: [.command])`，**仅在菜单栏面板展开时命中**；面板折叠时响应链上没有这个按钮）
- `⌘Q` 退出应用（`MenuBarContent.swift` 显式绑定 `.keyboardShortcut("q", modifiers: [.command])`，按钮内部调用 `NSApplication.shared.terminate(nil)`；**仅在菜单栏面板展开时命中**）。**云端数据源重构后，进程退出不再需要 `willTerminate → flushStickiesSave`**——便签数据已是服务端权威，窗口位置由 `StickyWindowController` 的 `didMove` / `didResize` 在每次触发时同步 `frameStore.save(...)` 到 UserDefaults，`save` 方法无缓冲

---

## 5. 构建和发布链路

### 5.1 本地脚本（`scripts/`）

所有脚本都**设计为可独立跑**，CI 对前 3 个脚本直接复用（`package-docker.sh` 是**本地开发专用**，CI 的 Docker 构建另走 `docker/build-push-action@v6` + buildx，不调用本脚本）：

| 脚本 | 产出 | 依赖 | 读 `VERSION` | CI 复用 |
|---|---|---|---|---|
| `package-web.sh` | `client/web/dist/` + 同步到 `server/internal/webui/dist/` | Node.js、npm | ❌ 不读，固定构建静态产物 | ✅ `build-web` job |
| `package-server.sh` | `dist/server/stickytodo-server-<ver>-<os>-<arch>[.exe]` × 7 + 汇总 `SHA256SUMS` | Go、跑过 `package-web.sh` | ✅ 默认 `dev`，通过 `-ldflags -X main.version=` 注入到 `/health` | ✅ `build-server` job |
| `package-mac-client.sh` | `dist/mac-client/stickytodo-<ver>-macos-universal.dmg`（**或** `--skip-dmg` 时 fallback 成 `stickytodo-<ver>-macos-universal.app.zip`）+ 汇总 `SHA256SUMS`；**注意**发布文件名带版本，但 **DMG / zip 内部的 `.app` bundle 恒为 `stickytodo.app`**（不带版本），这是用户拖到 `/Applications` 后在 Launchpad / Dock 里看到的名字，必须是干净品牌名。DMG 卷标（双击 DMG 后 Finder 窗口标题）为 `stickytodo <ver>` | Xcode **26.x**（完整 IDE）；CI 用 `macos-latest` runner **自带的默认 Xcode**（当前 runner 镜像上是 26.3.0），不再通过 `setup-xcode@v1` 强制锁版本，详见 §5.2 与 §7.7；DMG 打包优先 `create-dmg`（`brew install create-dmg`），缺失时 fallback 到系统自带 `hdiutil` | ✅ 默认 `dev`，**仅用于发布文件名 + DMG 卷标**，不改 App 内的 `CFBundleShortVersionString`，也不改 `.app` bundle 文件名 | ✅ `build-mac-dmg` job |
| `package-docker.sh` | 本地 Docker 镜像（当前平台单架构，不跨平台，`docker build` 而非 `docker buildx build`）| Docker daemon | ✅ 默认 `dev`，也作为镜像 tag | ❌ **CI 不调用**，CI 用 buildx 直推多架构 manifest |

脚本之间的依赖关系：

- 仅 `package-server.sh` 和 `package-docker.sh` 依赖 `package-web.sh` 的产物——它们把 server 编进二进制 / 镜像时会把 `server/internal/webui/dist/` 一并 `go:embed` 进去
- `package-mac-client.sh` **不依赖** web（macOS 是原生 Swift 客户端，不嵌 Web UI）
- `go build` 本身在 dist 缺失时也能通过（webui.go 会回退到内置的 placeholder 页）

另外 `package-web.sh` 的 npm 安装策略是：检测到 `package-lock.json` 走 `npm ci`（可复现）；没有 lockfile 才降级为 `npm install`。所以手动在 `client/web/` 下跑 `npm install` 是开发便利写法，CI / 打包脚本走的是 `npm ci`。

### 5.2 GitHub Actions

三个 workflow 文件分工：

- **`_build-all.yml`**（`on: workflow_call`）：reusable workflow，输入 `version` / `tag_name` / `prerelease` / `docker_image` / `tag_latest`，包含 6 个 job：
  1. `build-web`
  2. `build-server`（矩阵：linux × amd64/arm64/armv7、darwin × amd64/arm64、windows × amd64/arm64）
  3. `build-mac-dmg`（`macos-latest` runner（当前指向 `macos-26` / macOS Tahoe，自带 Xcode 26.3.0）+ `brew install create-dmg || true` + `package-mac-client.sh`）。**关于 Xcode 版本**：**当前不通过 `setup-xcode@v1` 锁版本**，直接走 runner 自带默认 Xcode。曾尝试锁到 26.4，但 runner 镜像预装 Xcode 最高只有 26.3.0，setup-xcode action 直接报 `Could not find Xcode version that satisfied version spec: '26.4'` 失败。跨 SDK 的 SwiftUI `.buttonStyle(.bordered)` 默认外观差异（便签加号按钮白底 vs 灰底）已经在 `StickyView.swift` 用 `.tint(.secondary)` + `.controlSize(.small)` 显式钉死覆盖，不再依赖 SDK 默认，所以不锁也能产出与本机一致的视觉。详见 §7.7
  4. `detect-docker-creds`（只有 3 行：读 `secrets.DOCKERHUB_USERNAME` 是否非空，输出 `have=true/false`；存在是因为 `secrets.*` 不能直接用在 `if:` 表达式里）
  5. `build-docker`（`needs: [build-web, detect-docker-creds]`、`if: needs.detect-docker-creds.outputs.have == 'true'`，用 `docker/setup-qemu-action` + `docker/setup-buildx-action` 推 `linux/amd64,linux/arm64,linux/arm/v7` 多架构）
  6. `publish-release`（`needs: [build-server, build-mac-dmg, build-docker]`；条件是多行 `if:` 表达式，容忍 `build-docker` 在没 secrets 时被 `skipped`，但 server / mac 任一失败仍会中止；用 `softprops/action-gh-release@v2` 把 `build-server` / `build-mac-dmg` 的 artifact 挂到 Release）
- **`release-tag.yml`**（`on: push: tags: ['v*']`）：调用 `_build-all.yml`，`docker_image=docker.io/hanxi/stickytodo`、`tag_latest=true`、正式发布
- **`release-branch.yml`**（`on: workflow_dispatch`，带 `branch` 输入）：先跑 `cleanup-old-release` job，**三阶段兜底**删同名旧 release（①`gh release delete --cleanup-tag` → ②降级为 `gh release delete` + `git push --delete origin <tag>` → ③容忍 tag/release 都不存在的首次运行），再调 `_build-all.yml` 生成 prerelease，`tag_latest=false` 确保不会覆盖 `:latest` 镜像

所需 secrets：`DOCKERHUB_USERNAME` / `DOCKERHUB_TOKEN`。**不是"不配就跳过 push"，而是"不配就完全跳过 `build-docker` 这个 job"**（镜像不会构建、不会推送）；其他产物不受影响。完整手册见 [docs/RELEASE.md](./docs/RELEASE.md)。

### 5.3 产物矩阵

| 产物类型 | 命名 | 备注 |
|---|---|---|
| Server 二进制 | `stickytodo-server-<ver>-<os>-<arch>[.exe]` | 7 份：linux × (amd64/arm64/armv7)、darwin × (amd64/arm64)、windows × (amd64/arm64)；与同目录的 `SHA256SUMS` 汇总文件一起上传 |
| Mac 客户端 | 发布文件名：`stickytodo-<ver>-macos-universal.dmg`；DMG 卷标（挂载后 Finder 窗口标题）：`stickytodo <ver>`；DMG 内 `.app` bundle：**`stickytodo.app`**（无版本号） | universal（arm64 + x86_64）；脚本用 `codesign --force --deep --options runtime --sign -` 做 **ad-hoc** 签名（`--sign -` 等价短写 `-s -`），`--options runtime` 启用 Hardened Runtime 以便将来可平滑切到开发者 ID 签名；同目录一份 `SHA256SUMS`。**三套命名的分工**：① 发布文件名要带 `<ver>` 供下载归档区分；② DMG 卷标要带 `<ver>` 方便用户知道自己挂的是哪个版本；③ `.app` bundle 名必须是干净的 `stickytodo.app`——这是用户拖进 `/Applications` 后在 Launchpad / Dock / Cmd+Tab 里永久看到的名字，绝不能混入发布文件名里的 `branch-main` / `v1.2.3` 之类噪声。历史 bug：曾把这三套名字合成一套，导致 DMG 双击开打是 "stickytodo branch-main (2849)" 这种脏窗口标题，且拖进 /Applications 后 App 图标下方显示 `stickytodo-branch-main-macos-universal`，非常不像正式应用 |
| Docker 镜像 | `docker.io/hanxi/stickytodo:<ver>`（正式 tag 时还会打 `:latest`）| 多架构 manifest：`linux/amd64` / `linux/arm64` / `linux/arm/v7`；镜像分发**不带** SHA256SUMS，完整性靠 registry digest |

`SHA256SUMS` 由 `package-server.sh` 和 `package-mac-client.sh` 在结尾处统一生成：优先 `sha256sum`（Linux），缺失时 fallback `shasum -a 256`（macOS 自带），产物文件名以相对路径写入同目录的 `SHA256SUMS`。Docker 镜像没有也**不应该**有这个文件——镜像完整性靠 registry 返回的 content digest（`sha256:...`）校验。

---

## 6. 验证和回归

任何一次改动后都应从仓库根执行以下两条命令，均以退出码 0 结束：

```bash
# 1) 后端端到端冒烟（36 步，覆盖 /health、login、todo CRUD、complete、reopen、
#    history、tags、软删、恢复、audit、sticky-notes CRUD、401 & 400 & 404 分支，
#    以及 Step 33-36 的 WebSocket 回归：
#      Step 33: /api/ws 未在 2s 内发 auth 帧 → close 4401
#      Step 34: auth with invalid token → close 4401
#      Step 35: auth with valid token → 收到 {"type":"ready"} 帧
#      Step 36: REST 触发 POST /api/todos → ws-probe 确认收到 todo.created 实时推送）
#    脚本启动时会 `go build ./scripts/ws-probe` 产出一个临时 WS 探针二进制。
#    前置：另起终端 `cd server && export TODO_USERNAME=admin TODO_PASSWORD=test123 && go run ./cmd/todo-server`
#    （smoke.sh 的账号默认回退值是 TODO_USERNAME=admin / TODO_PASSWORD=test123，必须与 server 启动时一致；
#     .env.example 里的示例 TODO_PASSWORD=change-me-please 是 Docker 部署时改密用途，与本地 smoke 用的默认值不同）
#    （server/.env.example 里的 TODO_DATA_DIR=/data 是容器内路径，本地 `go run` 不要 source 它）
BASE_URL=http://127.0.0.1:8080 \
  TODO_USERNAME="${TODO_USERNAME:-admin}" \
  TODO_PASSWORD="${TODO_PASSWORD:-test123}" \
  ./server/scripts/smoke.sh

# 2) macOS 客户端 Xcode clean + build（Debug；ad-hoc 签名，仅本机可运行）
./client/scripts/build.sh
```

改 Web 时额外跑：

```bash
cd client/web
npm install           # 首次
npm run typecheck     # package.json 里是 "tsc -b --noEmit"
npm run build         # package.json 里是 "tsc -b && vite build"
```

改后端 Go 代码时：

```bash
cd server
go vet ./...
go build ./...
```

---

## 7. 开发注意事项

### 7.1 三端字段必须同步

后端 `server/internal/model/models.go` 里每个字段的改动（新增、改 JSON tag、改类型），**必须**在以下两处同步修改：

- `client/web/src/types/api.ts`
- `client/mac/stickytodo/Models/*.swift`

否则两端都会**抛错**（不是静默失败）：

- TypeScript：`any` 宽容类型不会报，但运行时读 `undefined.xxx` 会炸
- Swift：`JSONDecoder.decode(...)` 遇到类型不匹配会抛 `DecodingError`，APIClient 会把它包成 `APIError.decoding(...)` 返回给 UI（见 `Networking/APIClient.swift`），用户可见但不会崩溃

两边都建议改 DTO 后手动过一遍接口单测或 smoke 流程。

### 7.2 React Hooks

`HistoryView` 曾经踩过"把 `useQuery` 放到条件三元里"的坑，导致 query state slot 错位。**所有 hooks 必须无条件调用**，想切不同数据源就在 `queryKey` / `queryFn` 内部用 `if`。

### 7.3 zustand persist 注意事项

**云端数据源重构后，`stickyStore` 已删除**——便签数据由 `/api/sticky-notes` 配合 TanStack Query 管理，无需前端持久化。当前仅 `authStore`（token + username）和 `uiStore`（深色模式）走 `zustand/persist`。

这两个 store 结构都非常简单（字符串 / 枚举），目前**没有 `version` + `migrate` 的需求**。但如果未来给它们加字段，需要遵守：

1. 浏览器端的持久化一定会存在老版本，不要假设 `localStorage` 里的旧数据结构完整
2. 结构性不兼容变更（重命名字段、拆对象、必填字段）必须配合 `version: N` + `migrate: (persisted, fromVersion) => ...` 升级；additive 变更（新增可选字段）也建议在 `onRehydrateStorage` / 自定义 `merge` 里做一次性校验
3. 如果将来 `stickyStore` 回归（例如需要离线缓存），要重新参考老版本 git 历史里 `normalizeSticky` 的兜底模式，而不是在 reducer 里假设字段都齐全

### 7.4 不要把 WebUI 当静态资源扔出去

`server/internal/webui/dist/` 是 **build 产物镜像目录**，它**不应该**出现在 git 里（除了 `.gitkeep`）。开发时如果想本地跑带 Web 的 server：

```bash
./scripts/package-web.sh      # 先构建 web 并同步（加 ./ 前缀，避免误走 PATH）
cd server && go run ./cmd/todo-server
```

没跑 `package-web.sh` 也能 `go run`，只是 `/app/` 会返回 placeholder 页提示你去构建，不会崩。

### 7.5 交叉编译纪律

- 后端绝对不要引入需要 CGO 的依赖（例如原 `mattn/go-sqlite3`），`go.mod` review 时要看一眼
- Dockerfile 必须保持 `CGO_ENABLED=0`（静态链接）+ `FROM gcr.io/distroless/static-debian12:nonroot`（当前运行阶段基础镜像，见 Dockerfile 中唯一一个非 `--platform=$BUILDPLATFORM` 的 `FROM ... AS runtime` 行），实测本地 amd64 镜像约 40MB；换成 alpine/ubuntu 基础镜像会显著变大且拖慢冷启动
- Mac 客户端打 DMG 必须 universal——`package-mac-client.sh` 里同时传 `ARCHS="arm64 x86_64"` **和** `ONLY_ACTIVE_ARCH=NO`（两者必须成对，只传 ARCHS 不够；脚本最后还会 `lipo -archs` 核对产物确为 `arm64 + x86_64` fat binary），否则 Intel Mac 用户会拿不到可执行的 App

### 7.6 版本号来源

- CI 里版本来自 `github.ref_name`（tag 名）
- 本地脚本来自 `$VERSION` 环境变量，`package-server.sh` / `package-mac-client.sh` / `package-docker.sh` 均默认 `dev`；`package-web.sh` 不读 `VERSION`（静态产物）
- 后端二进制启动时 `/health` 返回的 `version` 由 `-ldflags "-X main.version=..."` 在 build 时注入，用户能实时看到
- **Mac 客户端版本号的限制**：当前 `MARKETING_VERSION` 在 `stickytodo.xcodeproj/project.pbxproj` 里**硬编码为 `1.0`**，`package-mac-client.sh` 不会修改 Info.plist，因此 DMG 里的 App "关于"信息永远显示 `1.0`；外部可见的版本号只有**产物文件名**（`stickytodo-<VERSION>-macos-universal.dmg`）。如果未来需要把 `$VERSION` 真正写进 App Bundle，需要在 `package-mac-client.sh` 的 xcodebuild 阶段额外改 pbxproj 的 `MARKETING_VERSION` 或用 `PlistBuddy` 改生成后的 `*.app/Contents/Info.plist`

### 7.7 macOS 客户端 Xcode / SDK 版本一致性

**当前策略**：CI `build-mac-dmg` job **不锁 Xcode 版本**，直接使用 `macos-latest` runner 镜像自带的默认 Xcode（写作本章时是 **26.3.0**，Xcode 根随镜像每周更新漂移）；维护者本机为 Xcode 26.4（macOS 26.4 Tahoe）。两端 Xcode 次要版本号不完全一致是**可接受的**，详见下方说明。

**决策历史**：

1. **老策略（`macos-14` + Xcode 15.4）**：CI 和本机 Xcode 主版本不同，SwiftUI `.buttonStyle(.bordered)` 的默认外观填充差异明显——便签加号按钮在 CI DMG 里是**白底**、本机 Debug 是**浅灰底**。典型的"跨 SDK 视觉漂移"
2. **中间尝试：用 `maxim-lobanov/setup-xcode@v1` 锁到 26.4**：想同时解决 runner 默认版本漂移 + 两端 Xcode 对齐两件事。但 runner 镜像 2026-04 当前预装 Xcode 最高只到 26.3.0，action 直接以 `Could not find Xcode version that satisfied version spec: '26.4'` 失败（见 `_build-all.yml` 该 job 的注释）
3. **当前：不锁 Xcode + 在源码层显式钉死外观参数**：放弃"锁 Xcode 版本"这条纪律，改用"关键控件不依赖 SDK 默认外观"来防漂移。具体：便签加号按钮（`Views/StickyView.swift` 的 `titleBar` 内）显式追加 `.tint(.secondary)` + `.controlSize(.small)`，让按钮填充与尺寸档位不依赖 SDK 默认

**为什么现在可以不锁**：跨 Xcode 26.x 次要版本（26.0/26.1/26.3/26.4）的 SDK 变化远小于 Xcode 15 → 26 那种主版本跳跃，同时所有曾踩过坑的控件都已经手工加了显式 modifier。只要 runner 默认 Xcode ≥ 26.0（macOS 26 SDK），产物视觉就应和本机一致。

**仍然要守的纪律**：

1. **关键按钮的显式 modifier 不能删**。当前至少 `StickyView.swift` 的加号按钮依赖 `.tint(.secondary)` + `.controlSize(.small)`，不要为了"代码简洁"把它们拿掉——一旦 runner Xcode 默认版和本机分叉，视觉分歧会立刻回归。未来如果发现 `MenuBarContent.swift` / `SettingsView.swift` 里的 `.bordered` / `.borderedProminent` / `.menuStyle(.borderlessButton)` 在 CI 产物里和本机不一致，第一反应应是**给它们也加显式外观 modifier**，不是回头锁 Xcode
2. **不要主观升级本机 Xcode 到比 runner 高太多**。runner 镜像更新大约每 2-3 周一次；维护者本机和 runner 的 Xcode 主版本号应保持一致（都 26），次要版本差距 ≤ 2 个点位以内。偏差超过就人工抽检一次 CI 产物 DMG
3. **若未来 CI 产物又出现视觉漂移**（例如 Xcode 27 发布后 runner 优先更新、而本机还没升），回退策略有两个，**按优先级**：
   - **优先**：继续在源码层对涉事控件追加显式 modifier（最稳）
   - **其次**：才考虑在 `_build-all.yml` 的 `build-mac-dmg` job 里恢复 `setup-xcode@v1`，但必须先 `gh api /repos/actions/runner-images/contents/images/macos` 或看 runner 镜像 release note 确认要锁的版本确实预装了，再写到 `xcode-version` 里，否则会重蹈"26.4 找不到"的覆辙

---

## 8. 常见开发场景

**加一个业务字段（例如给 TODO 加 `assignee`）**：

1. `server/internal/model/models.go` 加字段 + JSON tag（`AutoMigrate` 会自动建列）
2. `server/internal/repository/` **通常无需改动**——`TodoRepo.Update(ctx, id, fields map[string]interface{})` 是动态 `Updates(map)`，新增字段只要 handler 把它放进 map 就行；仅当需要新增按该字段查询/排序的专用方法时才改 repo
3. `server/internal/service/` 如果要做字段级校验就加校验；**审计 diff 无需特殊处理**——`audit_service.go` 把整块变更 struct JSON 化写入 `Detail`，新字段自动被记录
4. `server/internal/handler/` DTO 映射（请求体绑定 + 响应序列化），并把新字段加入 Update handler 构造的 map
5. `client/web/src/types/api.ts` 加字段
6. `client/mac/stickytodo/Models/Todo.swift` 加字段（`Codable`，和 JSON tag 同名即可）
7. 跑 `smoke.sh` 确认不破坏现有流程

**加一个 API 端点**：

1. `server/internal/service/` 先写纯业务逻辑
2. `server/internal/handler/` 加 Gin handler
3. `server/internal/router/router.go` 注册路由——鉴权接口挂到 `authed := r.Group("/api")` 下；无需鉴权（如 `/api/login`）直接挂到 `r.` 上
4. `server/scripts/smoke.sh` 里补一步回归
5. 客户端各补一个调用方法：
   - Web：`client/web/src/api/client.ts` 加一个 `api.xxx` 方法 + 必要时 `src/api/queryKeys.ts` 加 cache key + `src/types/api.ts` 加 DTO
   - macOS：`client/mac/stickytodo/Networking/Endpoints.swift` 加 URL 构造器 + `APIClient.swift` 加 `async throws` 方法

**加一个 WS 事件类型**（例如"todo.archived"）：

1. `server/internal/ws/event.go` 新增常量 `EventTodoArchived = "todo.archived"`（名称用 `<resource>.<动词过去式>` 格式保持与现有事件一致）
2. `server/internal/service/broadcaster.go` 的 `EventBroadcaster` interface 新增一行签名 `BroadcastTodoArchived(todo any)`（或者根据 payload 形态选 `(id uint)`）——**interface 是权威入口**；`nopBroadcaster` 也要补一行空实现，否则 `var _ EventBroadcaster = nopBroadcaster{}` 编译报错
3. `server/internal/ws/adapter.go` 的 `*HubBroadcaster` 上补实现（`var _ service.EventBroadcaster = (*HubBroadcaster)(nil)` 这行会在漏实现时编译报错兜底），方法体调用 `NewResourceEvent(EventTodoArchived, todo, b.logger)` 或 `NewDeleteEvent(...)`
4. `server/internal/service/todo_service.go`（或 sticky_service.go）在对应写操作的 repo 返回成功之后直接 `s.broadcaster.BroadcastXxx(after)`——**现有 service 层没有显式事务包裹**（`todo_service.go:116,225,247,268,285,303` 全部是"repo 返回即广播"的调用形态），GORM 默认每次 `Updates()` 就是一次隐式事务，本步不引入事务也不要为新事件单独开事务，保持既有调用风格
5. `server/scripts/smoke.sh` 在 Step 33-36 附近加一步 `ws-probe` 校验：触发新操作 → 确认 ws-probe 收到对应 type 的事件
6. 客户端订阅：
   - Web：`src/hooks/useRealtimeSync.ts` 的事件 `switch` 里新增 case，决定 invalidate 哪些 queryKey
   - macOS：`AppState.handleRealtimeEvent` 的 `switch` 里新增 case（sticky.* 直接 merge；todo.* 一般只需 `postTodoNotification` 扇出给 ViewModel 去抖 refresh，不需要在 ViewModel 里为每种事件单独分支）
7. **不要**给 `todo.updated` 这种"已存在的宽泛事件"再拆分细粒度子事件（如 `todo.title_changed`）——现有架构假设客户端收到 `todo.updated` 就无脑全量 refetch，增加细粒度事件只会让广播膨胀，不会减少客户端请求数

**改 Web UI**：

- 组件在 `client/web/src/components/`，业务数据用 TanStack Query 拿
- 纯前端状态（便签位置、折叠状态、深色模式）放 Zustand
- 跑 `npm run dev`，Vite 会代理 `/api` 到后端，本地前后端分离联调

**发版**：见 [docs/RELEASE.md](./docs/RELEASE.md)。简化流程：`git tag v1.2.3 && git push --tags`，CI 会自动把 7 份二进制、DMG、Docker 镜像都打好并挂到 Release。
