# AGENTS.md — stickytodo 项目架构

面向开发者 / AI 代理的**工程指南**。安装和使用请看 [README.md](./README.md)。

本文目标：让任何一个新上手的人（或 agent）在读完本文后，就能独立完成改代码、跑测试、发版本这三件事，而不需要再去翻代码猜架构。

---

## 1. 全局视图

stickytodo 是一个 **C/S 架构** 的单账号 TODO 工具，一个后端配三种客户端：

```
┌──────────────────────┐    HTTPS / HTTP           ┌──────────────────────────┐
│ macOS 菜单栏客户端    │ ─── REST + JWT ───────▶   │                          │
│ (Swift / SwiftUI)    │                           │   stickytodo-server       │
└──────────────────────┘                           │   (Go + Gin + GORM       │
                                                   │    + glebarez/sqlite)     │
┌──────────────────────┐                           │                          │
│ Windows 桌面客户端    │ ─── REST + JWT ───────▶   │   ┌────────────────────┐ │
│ (Win32 + C++/WinRT + │                           │   │ go:embed 内嵌 Web   │ │
│  Direct2D 自绘 UI)   │ ◀── /api/ws ─────────────│   │ (dist/ → /app/)    │ │
└──────────────────────┘                           │   └────────────────────┘ │
                                                   │                          │
┌──────────────────────┐                           │                          │
│ 浏览器 Web UI         │ ─── REST + JWT ───────▶   │                          │
│ (React + Vite,       │                           │                          │
│  内嵌到 server 二进制)│ ◀── GET /app/ ───────────│   SQLite (单文件)         │
└──────────────────────┘                           └──────────────────────────┘
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
│   ├── win/                   # Windows 原生客户端（Win32 + C++/WinRT + Direct2D 自绘 UI）
│   │   ├── CMakeLists.txt     # 主构建脚本 + BUILD_TESTS 开关
│   │   ├── CMakePresets.json  # debug / release 两个 preset，binaryDir = build/<preset>
│   │   ├── vcpkg.json         # 依赖清单（nlohmann-json / cppwinrt；tests feature 追加 gtest）
│   │   ├── src/
│   │   │   ├── main.cpp / App.{h,cpp}   # 进程入口 + 全局单例；WM_STICKYTODO_* 消息路由中枢
│   │   │   ├── core/          # AppState / HttpClient / WebSocketClient / CredentialStore
│   │   │   │                  # (Credential Manager) / FrameStore (HKCU) / Timer
│   │   │   ├── models/        # Todo / StickyNote / Filter / AuditLog（POD，与后端 JSON 对齐）
│   │   │   ├── codec/         # JsonHelper（DTO ↔ nlohmann::json）+ StickyCodec（hex/filter 编解码）
│   │   │   ├── ui/            # D2DRenderer / Theme / Controls（自绘控件）/ Preferences（HKCU 偏好）/
│   │   │   │                  # StickyWindow / SettingsWindow / FilterEditor / TrayIcon
│   │   │   └── res/           # app.rc + ico
│   │   └── tests/             # gtest 单测（codec + models JSON 往返）
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
│   │   ├── ...（同上）
│   └── scripts/build.sh       # macOS 客户端本地回归
├── installer/
│   └── setup.iss              # Inno Setup 6 脚本（Windows 安装包；AppId 固定 GUID，勿改）
├── scripts/                   # 打包脚本（本地可单独跑，CI 也复用）
│   ├── package-web.sh         # 构建 web + 同步到 server/internal/webui/dist
│   ├── package-server.sh      # 7 份跨平台二进制 + SHA256SUMS
│   ├── package-mac-client.sh  # xcodebuild universal → codesign → DMG
│   ├── package-win-client.sh  # cmake --preset release + ctest + Inno Setup → zip + setup.exe + SHA256SUMS
│   ├── package-docker.sh      # 单架构 docker build（多架构交给 CI）
│   └── generate-icons.sh      # 从 assets/branding/*.svg 派生：Mac AppIcon.appiconset + MenuBarIcon.imageset（template）+ AppIcon.icns + Web favicons + Windows 多分辨率 stickytodo.ico
├── .github/workflows/
│   ├── _build-all.yml         # 可复用 workflow，7 job：build-web / build-server(matrix) / build-mac-dmg / build-win-client / detect-docker-creds / build-docker(qemu+buildx) / publish-release
│   ├── release-tag.yml        # push tag v* 自动走正式发布
│   └── release-branch.yml     # workflow_dispatch 分支预发布（先删旧 release）
└── docs/RELEASE.md            # 发版操作手册
```

> 上面的 `client/web/` 子树已折叠为省略号，完整子目录保持与之前一致（`src/api` / `hooks` / `store` / `types` / `lib` / `components` / `views` + `vite.config.ts`）。Windows / macOS / Web 三端在**功能** 上是对等的，只是实现技术栈不同。

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

- 心跳：服务端每 `pingPeriod = 30s` 主动发 WebSocket ping（`client.go`），客户端必须回 pong；服务端 `SetPongHandler` 在收到 pong 后把读超时重置 `pongWait = 60s`（注意：服务端**没有**注册 `SetPingHandler`，gorilla/websocket 默认 handler 只自动回 pong、**不重置 readDeadline**——所以让连接保活的唯一路径是"服务端 ping → 客户端 pong"，客户端单向发 ping 无法延续读超时）。60s 内没拿到任何 pong 就会触发 `SetReadDeadline` 过期 → 读循环 EOF → Hub 移除客户端。浏览器 `WebSocket` API 会自动响应 ping，无需应用代码处理；`URLSessionWebSocketTask`（macOS）也会自动处理服务端 ping 帧，但 `RealtimeClient` 额外跑了一个 15s 的客户端侧 `sendPing`——它的作用是**探测本端到服务端的链路是否仍活着**（弱网下 receive 可能长时间 hang 而不抛错），一旦发送失败就能通过 completion / 下一次 receive 报错尽快触发重连。Windows 端 `core::WebSocketClient`（基于 `winhttp.dll` 的 `WinHttpWebSocket*` API）同样采用**被动响应 ping + 主动发 ping 兜底**的双保险：WinHTTP 对控制帧会自动回 pong，本端也每 15s 调 `WinHttpWebSocketSend` 发一帧 ping——与 macOS `RealtimeClient.sendPing` 同义，都是为了让 `WinHttpWebSocketReceive` 能在网络黑洞时尽快拿到 ERROR 而不是无限 hang
- CheckOrigin：`ws.Handler` 的 `makeOriginChecker` 比 REST 的 CORS 多一条"同源放行"——因为浏览器对 WS 握手仍会带 Origin（RFC 6455 §10.2），而 router 的 CORS 中间件依赖浏览器同源时省略 Origin
- Hub 不缓冲历史事件：客户端**必须**在 reconnected 时自行全量 refetch（Web 端 `useRealtimeSync` 的 `'reconnected'` signal、macOS 端 `RealtimeClient` 的 `.reconnected` signal、Windows 端 `AppState::OnWebSocketReconnected()` 都会触发这一点）
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

### 4.3 Windows 客户端（client/win/）

技术栈：**C++20 + Win32 + Direct2D + DirectWrite + WinHTTP + nlohmann-json + Ninja + vcpkg**（`CMakeLists.txt` 里 `CMAKE_CXX_STANDARD 20` + `CXX_STANDARD_REQUIRED ON`；依赖清单见 `client/win/vcpkg.json` = `nlohmann-json` + `cppwinrt`（后者仅为保留 C++/WinRT 投影可用性，当前代码**尚未使用** `<winrt/...>` headers；`CMakeLists.txt` 的 `target_link_libraries` 中的 `windowsapp` 同理 — 作为未来引入 C++/WinRT API 时的预留入口），测试额外加 `gtest`；vcpkg baseline 固定在 `vcpkg-configuration.json`，**不要**随便升 baseline，详见 §7.8）

**为什么是自绘 UI（不是 WPF / WinUI / Qt）**：

- WPF / WinUI 强依赖 .NET 运行时或 WindowsAppSDK 框架包，分发麻烦（要么装 .NET，要么走 MSIX/app attach），与"单 exe 便携版"目标不符
- Qt 虽然静态链接可做单 exe，但整体产物 >50MB 且 LGPL 合规要暴露 obj 文件，不适合 MIT 项目的轻量定位
- Win32 + Direct2D 自绘：exe 典型 2~3MB，只依赖系统 DLL（`d2d1.dll` / `dwrite.dll` / `winhttp.dll` 在 Windows 10 20H1+ 全部内置），无运行时依赖。代价是**自己画一套控件**——这也是 `ui/Controls.{h,cpp}` 存在的原因（Button / CheckBox / TextBox / Label / ScrollView 自绘，不用系统 BUTTON / EDIT）

Bundle 和命名（真值来自 `client/win/CMakeLists.txt` + `src/res/app.rc` + `src/res/app.manifest`）：

- 可执行名：`stickytodo.exe`（CMakeLists 的 `add_executable(stickytodo WIN32 ...)`；`WIN32` 声明让链接器生成 GUI 子系统而非控制台）
- 目标平台：Windows 10+（`app.manifest` 的 `<compatibility>` 段显式声明 supportedOS = Windows 10 GUID + Windows 11 GUID）。代码里**没有**显式定义 `_WIN32_WINNT`，依赖 Windows SDK / MSVC 工具链的默认值（安装 Windows SDK 10.0.x 时默认 `0x0A00`=Windows 10）——如果未来要把最低版本卡得更严（例如强制 20H1+ 以使用 `ID2D1DeviceContext6`），应该在 `CMakeLists.txt` 用 `target_compile_definitions(stickytodo PRIVATE _WIN32_WINNT=0x0A00 WINVER=0x0A00)` 显式锁定
- 产品版本号：`src/res/app.rc` 里通过 `#define VER_MAJOR/VER_MINOR/VER_PATCH/VER_BUILD` **硬编码** `1,0,0,0`，`StringFileInfo` 的 `FileVersion` / `ProductVersion` 字符串也硬编码为 `"1.0.0.0"`——与 macOS 客户端**同一 corner case**：`package-win-client.sh` 不改 `.rc` 文件，所以 exe 的"属性 → 详细信息"永远显示 `1.0.0.0`；外部可见的版本号只有**产物文件名**（`stickytodo-<VERSION>-windows-<x64\|arm64>.zip` / `stickytodo-setup-<VERSION>-<x64\|arm64>.exe`）。如果未来需要把 `$VERSION` 真正写进 PE 资源，需改造 `app.rc` 为 CMake `configure_file` 模板 + 在 CMakeLists 解析 `APP_VERSION` 拆 4 段数字
- 凭据存储：**Windows Credential Manager** 通过 `wincred.h` 的 `CredWriteW/CredReadW/CredDeleteW`（封装在 `core/CredentialStore`）。**target name 按用户名动态拼接**：`L"stickytodo/" + Utf8ToWide(username)`（`MakeTargetName`），不是单一常量；另有一个独立常量 `kLastUserTarget = L"stickytodo/__last_user__"` 专门存"最近登录用户名"用于登录表单预填。与 macOS `KeychainStore.service = "com.hanxi.stickytodo"` 等价的本机安全区
- 本机偏好：**HKCU\Software\stickytodo** 下的 REG_DWORD 键，见 `ui/Preferences.cpp`。当前两个键：`skipTodoDeleteConfirm` / `skipStickyDeleteConfirm`（对齐 macOS 的 `todo.skipDeleteConfirm` / `sticky.skipDeleteConfirm` `@AppStorage` 键）
- 窗口位置：`core/FrameStore` 存在 **`%LocalAppData%\stickytodo\frames.json`**（单个 JSON 文件，不是注册表；kv 结构 `{<stickyId>: {x, y, width, height}}`，见 `FrameStore::PersistAll`）——与 macOS `UserDefaults` 的 `stickytodo.frames` 语义对等，**本机 UI 偏好、不跨端同步**。选 JSON 而非注册表：便于 `StickyWindowManager` 启动时一把读完整个 map，且用户手工编辑 / 备份更友好；代价是多一个"文件目录不存在时要先 mkdir -p"的边界处理
- DPI：`src/res/app.manifest` 同时声明两个 DPI 键，遵循 MSDN 的 per-monitor-v2 兼容写法——新 schema 下的 `<dpiAwareness xmlns="...SMI/2016/WindowsSettings">PerMonitorV2</dpiAwareness>` 给 Windows 10 1607+ 用；同时保留老 schema `<dpiAware xmlns="...SMI/2005/WindowsSettings">true/pm</dpiAware>` 作为 1607 之前的 Windows 10 早期构建的 fallback（虽然 installer 的 `MinVersion=10.0.19041` 已经把这些早期版本排除，但 portable zip 用户可能跳过 installer 直接跑 exe，所以保留 fallback 无害）。manifest 还显式声明 `<activeCodePage>UTF-8</activeCodePage>`（Windows 10 1903+ 特性，让 `A` 系列 Win32 API 自动用 UTF-8，避免路径含全角时出乱码）。所有坐标系在 `D2DRenderer` 里按 `GetDpiForWindow(hwnd) / 96.0f` 缩放；`Theme.h` 里的常量（`kFontSizeBody`、`kCheckboxSize` 等）都是 **96-DPI 基准值**，渲染时统一乘 `dpi`

模块职责：

- **App / main.cpp**：`WinMain` 进程入口 + `App` 全局单例。App 持有 `HINSTANCE` / `D2DRenderer` / `AppState` / `TrayIcon` / `SettingsWindow` / 一个 `std::unordered_map<std::string, std::unique_ptr<StickyWindow>>`（stickyId → 窗口）。**WM_STICKYTODO_* 消息路由中枢**就在这里：`PostMessageToAllStickies` 广播到所有便签窗（Refresh 用）、`PostMessageToSticky(id, msg)` 精准路由（UPSERTED/DELETED 用）。`OnStickyWindowDestroyed(stickyId)` 在 `StickyWindow::WM_DESTROY` 时把 unique_ptr 从 map 里 erase——**这是内存正确性的关键**：WM_STICKYTODO_STICKY_DELETED 不能在 handler 里直接 `delete this`，必须 `DestroyWindow(hwnd_)` 让 Win32 把消息泵内剩余消息处理完再触发 WM_DESTROY，再由 App 负责 erase
- **core/AppState**：`AppState` 聚合 JWT 态、StickyNote 列表、`HttpClient`、`WebSocketClient`、`CredentialStore`、`FrameStore`、`Timer`（用于去抖）。**WS 事件走"worker 线程 → PostMessageW → UI 线程"两段式路由**（`WebSocketClient::ReceiveLoop` 里的 `WinHttpWebSocketReceive` 是**同步阻塞**调用，其后同步触发的 `onEvent_` / `onSignal_` 是 `WebSocketClient` 自己定义的 `std::function`，**不是** WinHTTP 异步回调；但它们仍在 worker 线程执行，UI 数据结构必须回到 UI 线程才能安全 mutate，见 §7.8 UI 线程模型）：
  - worker 线程里 `WebSocketClient` 触发 `onEvent_` / `onSignal_` → `AppState::PostWsEventToUIThread` / `PostWsSignalToUIThread` 把 `WsEvent` 堆分配（`new WsEvent(event)`）后 `PostMessageW(uiThreadTarget_, WM_STICKYTODO_WS_EVENT/_SIGNAL, heap, 0)` 到 tray 的消息窗口
  - tray `WndProc` 收到消息 → 调用 `AppState::HandleWsEventOnUIThread` / `HandleWsSignalOnUIThread`（UI 线程）→ 释放堆对象并分派，真实分发规则（对齐 `AppState.cpp:207-289` 源码）：
    - `todo.*` 事件（`todo.created` / `todo.updated` / `todo.deleted`）→ `PostMessageToAllStickies(WM_STICKYTODO_REFRESH)`（广播给所有便签窗；窗口去抖 300ms 后 refetch 自己关心的 TODO 列表，不相关 filter 会丢弃，与 macOS 同语义）
    - `sticky.upserted` → `MergeStickyUpserted(data)` 更新 `stickies_` 缓存 + 触发 `onStickiesChanged_`（App 订阅后调 `SyncStickyWindows()` 开新窗）**同时**直接 `PostMessageToSticky(noteId, WM_STICKYTODO_STICKY_UPSERTED)` 让已存在的目标窗口重读自己的 title/bg/filter 并重绘
    - `sticky.deleted` → `MergeStickyDeleted(id)` 更新 `stickies_` 缓存 + 触发 `onStickiesChanged_`（`SyncStickyWindows` 会按"windows∉stickies 关窗"幂等对账）**同时**直接 `PostMessageToSticky(id, WM_STICKYTODO_STICKY_DELETED)` 让目标窗口自行 `DestroyWindow`。**两条路径幂等但不冗余**：`HandleWsEventOnUIThread` 的直接 post 是 low-latency 快路径（便签窗口立刻响应）；`SyncStickyWindows` 是 reconcile 兜底（保证启动时首次加载、以及 WS 事件漏处理的极端情况下状态最终一致）
- **core/HttpClient**：基于 `winhttp.dll` 的 REST 客户端；方法签名与后端一一对应，与 macOS `APIClient` / Web `api/client.ts` 契约一致。与 macOS `APIClient` 的 TodoFilterDTO 适配层对等——`codec/StickyCodec::FilterToJson` 负责 snake_case 的 JSON 字符串构造（后端约定），camelCase 的 C++ 结构体通过手写映射转换
- **core/WebSocketClient**：基于 `winhttp.dll` 的 `WinHttpWebSocket*` API。与 macOS `RealtimeClient` 的协议行为等价：首帧 auth（2s 服务端超时窗口内发）/ ready 帧 / 指数退避 `[1,2,4,8,16,30]s` / close code 4401 → `unauthorized` signal。**应用层不发任何 ping**——保活完全依赖服务端 30s WS ping（由 WinHTTP 在底层自动回 pong，不经过 `ReceiveLoop`），与 §3.3 ws 契约"除首帧 auth 外不接受任何上行业务帧 → close 4400"对齐（详见 §7.8）。WinHTTP 的 API 是**阻塞同步调用**，所以 `WebSocketClient` 内部跑在**独立工作线程**，事件经过 `PostMessageW` 送回 UI 线程（不能直接在 WinHTTP 工作线程里动 `AppState`——AppState 所有 mutator 假设 UI 单线程）
- **core/CredentialStore**：封装 `CredWriteW/CredReadW/CredDeleteW`，屏蔽 `CREDENTIAL` 结构体的繁琐填充和 UTF-16 转换。JWT 和 username 走**两个独立的 Credential Manager target**，都在系统 vault 里：①`L"stickytodo/" + username`（`MakeTargetName`）存 per-user 的 JWT blob ②`L"stickytodo/__last_user__"`（`kLastUserTarget` 常量）存"最近登录 username"用于下次启动时自动预填登录表单。**不使用** HKCU 注册表存 last-user，保持"敏感数据全部集中在 Credential Manager"的单一职责
- **core/FrameStore**：便签窗口位置**纯本机**持久化。与 macOS `FrameStore` 语义对等。**写路径**：`StickyWindow::OnResize`（`StickyWindow.cpp:250-259`，`WM_SIZE` 分派）和 `StickyWindow::OnMove`（`:261-263`，`WM_MOVE` 分派）内部都调 `SaveFramePosition()`（`:265` 起），最终落到 `FrameStore::Save(stickyId_, frame)`（`:277`），WM 分派点在 `:679-680`。`FrameStore::Save` / `Remove` / `PruneOrphans` 三个公开写接口都通过内部 `PersistAll(map)` helper 做"读全量 → 改内存 map → 写回整个 `frames.json`"的原子 flush（见 `FrameStore.cpp:69-99`），**无 in-memory 缓冲层**，每次写操作都立即 I/O 一次（`frames.json` 文件小、写频率低，不必引入 coalescing）。**读路径**：`bool StickyWindow::Create()`（`StickyWindow.cpp:103-148`）在调 `CreateWindowExW` 前先给 `x/y/w/h` 打**二分默认值**（`:117-118`）——位置 `x = y = CW_USEDEFAULT`（Win32 让 OS 自己级联摆放首次开的便签；**与 macOS 侧的语义对等但落点不同**：macOS `FrameStore.load(id:)` 文档注释明写"不存在时返回 nil，由调用方决定 fallback 策略（通常是 `StickyNote.defaultFrame` + 叠加偏移）"，Windows 侧把"fallback 策略"这一步委托给了 Win32 的 `CW_USEDEFAULT` 机制，效果类似），尺寸 `w = Theme::kStickyDefaultWidth, h = Theme::kStickyDefaultHeight`（Theme 常量）；随后 `:120-128` 调 `FrameStore::Load(stickyId_)`——签名 `std::optional<FrameRect> Load(...)`，缺失 sticky 或文件不存在返回 `nullopt`，调用处 `if (frame.has_value()) { x = ..; y = ..; w = ..; h = ..; }` 覆盖掉默认值。最终 `:130-137` 把 `x/y/w/h` 传入 `CreateWindowExW`
- **codec/JsonHelper**：`nlohmann::json ↔ 各 POD 模型` 的手写转换器，真实接口清单（见 `JsonHelper.h`）：**Todo** (`ParseTodo` / `ParseTodos` / `TodoToJson`) / **StickyNote** (`ParseStickyNote` / `ParseStickyNotes` / `StickyNoteToJson`——**整个便签的序列化归属在 JsonHelper 而非 StickyCodec**，StickyCodec 只处理便签内嵌的 `bg_color` 和 `filter` 两个子字段) / **Filter** (`ParseFilter` / `FilterToJson`——与 StickyCodec 的 `FilterToJson/JsonToFilter` 并存，二者都接 `models::Filter`；调用方通常直接走 `StickyCodec::FilterToJson` 以保持 sticky-内嵌 filter 的一致路径) / **AuditLog** (`ParseAuditLog` / `ParseAuditLogs`) / 通用 `SafeGet*` helpers（`SafeGetString` / `SafeGetInt` / `SafeGetUint64` / `SafeGetBool` / `SafeGetOptionalString`）。容错策略"脏数据兜底"——缺失字段用默认值，类型不匹配走 `SafeGet*` 的 `defaultVal` 兜底而不抛异常（与 Web `stickyCodec.ts` 的兜底哲学一致）
- **codec/StickyCodec**：便签内嵌**两组** JSON 子字段的双向转换（整张便签的序列化不在这里，在 `JsonHelper::ParseStickyNote/StickyNoteToJson`）：
  - **`bg_color` 组（4 个方法）**：`HexToBgColorJson(hex)` / `BgColorJsonToHex(json)` 做 hex 字符串（如 `"#FFEB8A"`）↔ 后端 `CodableRGBA` JSON（`{"red":1.0,"green":0.92,"blue":0.54,"alpha":1.0}`，浮点 0-1）的全链路；更底层的 `ParseBgColor(json) → RgbaColor` 和 `RgbaToJson(RgbaColor) → json` 直接吐 / 吃 `RgbaColor` POD（`StickyCodec.h:14-25` 定义，内含两个便利转换方法：`uint32_t ToColorRef() const` 转 Win32 `COLORREF`（`0x00BBGGRR` 格式，alpha 被丢弃）；`void ToD2DColor(float& r, float& g, float& b, float& a) const` 通过 4 个 out-param 吐出 `[0,1]` 范围的浮点分量，调用方再自行 `D2D1::ColorF(r, g, b, a)` 组装成 `D2D1_COLOR_F`）
  - **`filter` 组（2 个方法）**：`FilterToJson(Filter)` / `JsonToFilter(str)`：`models::Filter` POD ↔ snake_case JSON 字符串。**`StickyCodec::FilterToJson` 是全仓唯一的 filter 序列化调用点**（`StickyWindow.cpp:1301` 保存筛选条件时调用），`JsonHelper::FilterToJson`（`JsonHelper.cpp:170`）定义存在但**当前无任何调用者**——属于跨 codec 边界时遗留的重复实现，新代码务必走 StickyCodec 版本，避免两个实现分叉
  - **字段命名"r/g/b/a" vs "red/green/blue/alpha"**：macOS + Windows + Web 三端实际用的是**全拼** `red/green/blue/alpha`（macOS `APIClient.swift:398` + `StickyNote.swift:146` 的 `CodableRGBA(red:green:blue:alpha:)` 初始化器双重明证；Windows `StickyCodec.h:11-13` 注释 `/// Matches the backend/macOS "CodableRGBA" format: /// {"red":1.0,"green":0.92,"blue":0.54,"alpha":1.0}` 同样是全拼；Web `client/web/src/lib/stickyCodec.ts` 的 `hexToBgColorJSON`（`:45`）/ `bgColorJSONToHex`（`:59`）也序列化全拼），不是单字母缩写。**后端 Go 侧的语义微妙但不矛盾**：`server/internal/handler/sticky_handler.go:44` 的 DTO 和 `server/internal/model/models.go:73` 的 GORM 模型都把 `BgColor` 声明为 `string` 类型（`json:"bg_color"`），**只做透明字符串存储、不对 `{"red":...}` 做字段级反序列化**（`grep "type\s+\w*RGBA" server/ → 空`，没有对应 Go struct），但 `models.go:73` 注释 `// JSON: {red,green,blue,alpha}` 明确**约束了客户端该往字段里放什么 shape 的 JSON**——所以 Windows `StickyCodec.h:12` 注释里的 "**backend**/macOS CodableRGBA format" 指的是"后端**存储约定接受的 JSON shape**"（文档级契约），而不是"后端**有对应的 CodableRGBA 结构体**"（代码级契约）。结论：字段名一致性的**代码级契约**由 macOS ↔ Windows ↔ Web 三个客户端的 codec 共同维护，后端只是被动存取
- **ui/D2DRenderer**：`ID2D1Factory` / `IDWriteFactory` / `ID2D1HwndRenderTarget` 单例管理；所有 `StickyWindow` / `SettingsWindow` / `FilterEditor` 共享同一个 factory（COM 对象引用计数安全）
- **ui/Theme**：与 macOS `Color.*` Swift 常量逐条对应的 D2D `D2D1_COLOR_F` 函数（`TextPrimary()` / `CheckboxFill()` / `ButtonHover()` 等）；不用 static 全局是因为 D2D 颜色结构体在 header-only 初始化会和 MSVC `/ZI`（Edit and Continue）冲突
- **ui/Controls**：自绘控件库。`Button` / `CheckBox` / `TextBox` / `Label` / `ScrollView`。**每个控件三段式接口**：`rect`（布局）+ `Draw(rt, dw, dpi)`（渲染）+ `HandleMouse(msg, x, y)` 或 `HandleChar(c)` / `HandleKey(vk)`（输入）。`Button` 额外带 `selected: bool` 字段（用于分段选择器 / tab 等"持久选中"场景，独立于 Normal/Hover/Pressed 的 state 机），`CheckBox::Draw` 在 `!enabled` 时按 0.5 alpha 降透明度
- **ui/Preferences**：封装 HKCU 偏好读写，当前提供 `ShouldSkipTodoDeleteConfirm` / `SetSkipTodoDeleteConfirm` / `ShouldSkipStickyDeleteConfirm` / `SetSkipStickyDeleteConfirm` 四个函数。**任何新增的"记住用户选择"偏好都应加到这个文件，不要在各窗口 cpp 里埋匿名 namespace 的 Read/Write 辅助函数**
- **ui/StickyWindow**：单个便签窗口。`styleMask = WS_POPUP | WS_THICKFRAME`（无系统标题栏、可调尺寸），`SetWindowLongPtrW(GWL_EXSTYLE, ... | WS_EX_TOOLWINDOW | WS_EX_TOPMOST)` 置顶且不在任务栏显示。内部渲染分 4 区：
  - **标题栏**（`titleBarHeight_` 高，自绘）：一个临时布局的 `Label titleLabel`（只读文本，显示 `stickyNote_.title`；**当前实现没有点击进入重命名态的交互**——便签标题的修改仅通过服务端侧改动 + WS `sticky.upserted` 事件回灌，UI 层未实现本机编辑入口）+ `closeButton_`（`×` U+00D7，`rect = {W-32, 4, 24, 24}`，点击 → `App::CloseStickyWindow(stickyId_)`，仅关闭便签窗口，不删除便签数据）+ `settingsButton_`（`⚙` U+2699，点击 → `App::ShowSettings()`，永远可见）+ `plusButton_`（`+` U+002B，点击 → `BeginDraft()` 显示顶部 DraftTodoRow 并聚焦其输入框）+ `trashButton_`（`🗑` **U+1F5D1 WASTEBASKET emoji**——**故意选 emoji 而非 Segoe UI Symbol 内置字符**，与下文 TodoRow slot 3 的 `✖` 取舍相反；权衡理由：trashButton 是**hover-only** 组件，即便在老版 Windows 上 DirectWrite 找不到 Segoe UI Emoji 字体而 fallback 成 tofu，hover 这个交互信号已经独立传达了"破坏性操作"意图，见 `StickyWindow.cpp:379-385` 源码注释；**仅** `titleBarHovered_ == true` 时渲染且 rect 非零，否则 rect 置 `{0,0,0,0}` 使 hit-test 失败以避免误触；点击 → `DoDeleteSticky()` 删除便签本体）
  - **DraftTodoRow**：顶部待办新增入口（Enter 提交、Esc 取消）
  - **TodoRow 列表**：每行 hover 显示三个行动按钮（`RowHitTest::Zone` 枚举）。**三个 slot 的图标都刻意选 Segoe UI Symbol 内置字符（BMP 内码点）而非 emoji**（与标题栏 `trashButton_` 的 emoji 选择相反）——TODO 行是持续可见 UI，若 DirectWrite 找不到 emoji 字体 fallback 成 tofu 会非常显眼，见 `StickyWindow.cpp:536-538` 源码注释（"Use ✕ (U+2716) instead of the 🗑 emoji because Segoe UI does not ship emoji glyphs — DirectWrite would fall back and render tofu. ✕ is monochrome and always available in Segoe UI Symbol."）。此外**全部三个 slot 都受 `!todo.IsDeleted()` 守卫**（软删 TODO 只能在 `ActionDelete` 上触发恢复，另外两个 slot 压根不绘制）：
    - `ActionComplete` — 非软删时才绘制，图标按完成态二态切换：未完成 → `✓` (U+2713 CHECK MARK)，已完成 → `↺` (U+21BA ANTICLOCKWISE OPEN CIRCLE ARROW)；handler：`IsDone() → DoReopen` / else → `DoComplete`
    - `ActionEdit` — 非软删时才绘制，图标固定是 `✏` (**U+270F PENCIL**，**不是** U+270E LOWER RIGHT PENCIL——后者在老版 Windows 10 Segoe UI 上会 fallback 成 tofu，见 `StickyWindow.cpp:525-530` 源码注释)；handler：`BeginTitleEdit(rowIndex)` 进入 TodoRow 内联编辑态
    - `ActionDelete` — **三个 slot 中唯一对软删态也会绘制的**：非软删 → `✖` (U+2716 HEAVY MULTIPLICATION X) 触发软删，软删态 → `↶` (U+21B6 ANTICLOCKWISE TOP SEMICIRCLE ARROW) 触发恢复；handler 按 `IsDeleted()` 分派 `DoRestore` vs `DoDelete`
    - **点击 TODO 标题文本**（`RowHitTest::Zone::Title`）等价于点 `ActionEdit`，同样受 `!todo.IsDeleted()` 守卫
  - **FilterBar**（底部）：`filterButton_` 文本由 `BuildFilterSummary(filter_)` 动态构造，点击弹出 FilterEditor 模态
  - **WM_STICKYTODO_STICKY_DELETED 必须通过 `DestroyWindow(hwnd_)` 走常规消息泵路径，不要直接 delete**
- **ui/SettingsWindow**：设置窗口。**3 个 Tab** 对齐 macOS `SettingsView`：
  - 「设置」Tab：Base URL / 登录表单 / 测试连接 / 登出 / 通用偏好（两个删除确认 CheckBox）
  - 「历史」Tab：登录后拉取全局审计日志；未登录占位提示
  - 「关于」Tab：品牌 / 版本 / 项目链接
- **ui/FilterEditor**：模态筛选编辑器。与 macOS `FilterEditor.swift` 1:1 对齐：状态分段选择器（全部/未完成/已完成）、标签/关键词 TextBox、软删 2 CheckBox（`onlyDeleted` → `includeDeleted.enabled=false` 即时视觉联动）、页大小 stepper（10-200，step 10）、取消/重置/保存头部按钮。**Win32 模态的实现**：`CreateWindowExW(WS_VISIBLE)` + `EnableWindow(owner, FALSE)` + 局部 `GetMessage` 循环直到窗口销毁 + 返回前 `EnableWindow(owner, TRUE) + SetFocus(owner)`（这是 MSDN 定义的"模态消息循环"模式；不用 `DialogBox` 是因为我们的渲染路径是 Direct2D 自绘，不能走系统对话框模板）
- **ui/TrayIcon**：`Shell_NotifyIconW` 封装。图标菜单按鉴权态分化（`AppState::IsAuthenticated()` 判定，`ShowContextMenu` 内部分支）：
  - **未登录**：`Settings` / `Quit`
  - **登录后**：`New Sticky Note` / `Settings` / 分隔符 / `Logout` / `Quit`
  - 所有菜单项文案当前都是英文字面量（`AppendMenuW(hMenu, MF_STRING, ID_TRAY_..., L"...")`），与 `SettingsWindow` 的 Tab 名（`Settings` / `History` / `About`）保持一致
  - 交互：`WM_RBUTTONUP` / `WM_CONTEXTMENU` → `ShowContextMenu`；`WM_LBUTTONDBLCLK` → `App::ShowSettings()`；左键单击**无行为**
  - 当前图标**未使用** `NIF_GUID`（没有 `kTrayIconGuid` 之类的常量），仅靠 `(hwnd, uID)` 这对句柄 + ID 定位；如果未来要让 Windows 在用户升级版本后仍记住"图标已置顶"偏好，需要给它加一个 process-wide 恒定 GUID 并在 `NOTIFYICONDATAW.uFlags` 里开 `NIF_GUID`

模型（`src/models/`，共 4 个，与后端 `models.go` + `types/api.ts` + Swift `Models/` 一一对应）：`Todo.h` / `AuditLog.h` / `StickyNote.h` / `Filter.h`。**全部 POD（无虚函数、无继承）**，JSON 序列化通过 `codec/JsonHelper` + `codec/StickyCodec` 的手写函数——没用 `NLOHMANN_DEFINE_TYPE_INTRUSIVE` 是为了完全掌控字段缺失 / 类型不匹配时的兜底策略（宏版本遇到字段缺失会抛异常，我们要的是容错 → 默认值）

快捷键：

- Windows 桌面没有像 macOS `⌘,` 一样的系统级"打开设置"约定，SettingsWindow 的入口有 3 条：①**托盘图标右键菜单** → `Settings` ②**托盘图标双击**（等价于右键 → `Settings`，见 `TrayIcon::WndProc` 的 `WM_LBUTTONDBLCLK` 分派到 `App::ShowSettings`）③**便签窗口标题栏 `⚙` 按钮**（`settingsButton_` 永远可见，`onClick` 直接调 `App::ShowSettings()`）
- 便签窗口内部：`Enter`（在 DraftTodoRow 聚焦时）= 提交新 TODO；`Esc`（编辑中）= 取消；**没有任何 `RegisterHotKey` 调用的全局/系统级快捷键**（grep `RegisterHotKey` / `MOD_CONTROL` / `VK_N` 全仓返回空；与 macOS 的 `⌘N` 仅菜单展开时可触达是同一哲学——所有 sticky/todo 级操作都靠托盘菜单或便签窗口按钮触发）

**网络调用异步化** — 所有 UI 线程触发的 HTTP 调用都走 `HttpClient::Async*`，严禁在 UI 线程同步调 `HttpClient::*`：

- **问题背景**：WinHTTP 的 `WinHttpSendRequest` / `WinHttpReceiveResponse` 是同步阻塞的，且默认超时**非常宽松**（按 Microsoft 文档 <https://learn.microsoft.com/en-us/windows/win32/api/winhttp/nf-winhttp-winhttpsettimeouts>：`dwResolveTimeout = 0` **表示"无超时/infinite"而非 0 秒**，`dwConnectTimeout = 60 s`、`dwSendTimeout = 30 s`、`dwReceiveTimeout = 30 s`；累加最坏情况是**无限**，即使名字解析快也至少 **120 s**）。在 `Button::onClick` lambda 里同步调 HTTP 会直接冻结窗口消息泵，表现为 Windows 弹出"未响应"灰屏。macOS 侧同样场景靠 `URLSession` 的 completion handler 天然异步，不需要这层基础设施
- **超时值**：`HttpClient::DoRequest` 统一 `WinHttpSetTimeouts(session, 10000, 10000, 10000, 10000)` — resolve / connect / send / receive 各 10 s，兼顾内网快响和公网弱网。这是**会话级默认**，单个请求不再单独覆盖，保持行为可预测
- **UI-thread marshal 机制**：`core/UIThreadMarshal.{h,cpp}` 提供 `SetUIThreadTarget(HWND)` / `PostToUIThread(std::function<void()>)`。实现方式：把 lambda heap-allocate 后通过 `PostMessageW(target, WM_STICKYTODO_RUN_ON_UI, 0, (LPARAM)funcPtr)` 投递；`TrayIcon::WndProc` 收到 `WM_STICKYTODO_RUN_ON_UI` 时 `invoke() + delete`。**target HWND 选 tray**（不是 sticky / settings 的 HWND），因为 tray 是 App 生命周期内存活最长且唯一的窗口（sticky 可能被删，settings 可能被关）。`PostMessageW` 天然线程安全、天然按到达顺序串行化执行，不需要额外的 mutex 或队列
- **HttpClient Async API 形状**：每个同步方法都有配对的 `Async*` 变体，callback 签名是 `std::function<void(Result)>`（如 `AsyncListTodos(filter, [](std::optional<TodoListResult>){...})`）。内部实现统一样板：`std::thread([...]{ auto r = Sync版本(...); PostToUIThread([cb, r]{ cb(r); }); }).detach()`。**故意不用 `std::future` / `std::promise`**——MSVC `std::promise<void>` 在 WinHTTP 错误分支里存在生命周期陷阱（worker 抛 `future_error` 比 UI 线程读 future 更早时崩溃），`std::function` 回调简单可靠
- **Worker 线程 `detach()` 的代价**：进程退出时最多有 **~40 s**（4 × 10 s 超时预算）的 detached worker 残留。`App::Shutdown` 的闭环设计把这个代价吸收掉：
  1. step 2 `SetUIThreadTarget(nullptr)` — 后续 `PostToUIThread` 返回 false 直接丢弃 callback（worker 侧不会触碰已销毁对象）
  2. step 3 `drain` 把 tray HWND 消息队列里已排队的 `WM_STICKYTODO_RUN_ON_UI` 全部 `DispatchMessage`（lambda 执行 + 堆对象释放，防止内存泄漏）
  3. step 4-8 按 tray → settings → stickies → state_ → D2D 的顺序 reset
  4. 最后 `g_app = nullptr`（`App.cpp:224`）。detached worker 即使在 `WinMain` 返回后才醒来也只会读到 `g_app == nullptr` 而 early-return，进程整体被 `ExitProcess` 清理，不 join 是**刻意选择**（见 `HttpClient.cpp` async 实现注释）
- **UI 回调的 `this` 捕获安全性**：三种 guard 模式按窗口类型区分，**任何新加的异步回调必须选其一**：
  - **StickyWindow（窗口数量不定、生命周期短、可被 WS 推送销毁）**：`std::shared_ptr<std::atomic<bool>> alive_` 字段（声明在 `StickyWindow.h:200`），构造时默认 true、析构函数**第一行**置 false。回调 capture `[this, alive = alive_, ...]`，入口先 `if (!alive->load()) return;`。shared_ptr 保证 atomic 的存储在回调执行前不会被释放（即使 StickyWindow 本体已析构）。二次守卫 `if (!hwnd_) return;` 作为 defense-in-depth，但**真正起作用的是 `alive->load()`**——`hwnd_` 的读取发生在 alive==true 分支内，此时对象还活着，语义安全
  - **StickyWindow LoadData 特化 — 请求代 token**：`loadDataGeneration_` 单调计数器，`LoadData()` 入口 `uint64_t myGen = ++loadDataGeneration_`，callback capture `myGen` 并在 alive 守卫后判 `if (myGen != loadDataGeneration_) return;`。**必要性**：`ShowFilterEditor` 失败回滚路径会触发第二次 `LoadData()`，如果没有代号，两次 `AsyncListTodos`（不同 filter）的 callback 可能**乱序落回**，导致 `todos_` 与 `filter_` 内容错位
  - **SettingsWindow（单例，App 持有 `unique_ptr`）**：guard `auto* app2 = GetApp(); if (!app2 || app2->GetSettingsWindow() != self) return;`。原理：`App::Shutdown` step 3 drain 先清空所有已排队回调，再 step 5 `settings_.reset()`——因此回调真正执行时 SettingsWindow 要么还活着（`GetSettingsWindow() == self`），要么整个 App 已经 `g_app = nullptr`（`GetApp()` 返回 null）
  - **TrayIcon（单例，菜单命令触发但无 `this` 依赖）**：NEW_STICKY 的 `UpsertStickyAsync` callback **不捕获 `this`**，只捕获值类型数据（`stickyId` by value）+ 通过 `stickytodo::GetApp()` 重查拿 state。g_app 在 Shutdown 最后置 null，单独通过 `GetApp()` null check 就足够
- **乐观 vs 悲观的分工**（回答"为什么 StickyWindow 10 处调用不是一刀切同一策略"）：
  - **写操作（CreateTodo / UpdateTodo / Complete / Reopen / Restore / Delete / UpsertSticky）→ 乐观 + 回滚**：用户点击后立即本地改 `todos_` / `filter_` 并重绘，HTTP 异步飞；callback 成功则用服务端返回的 Todo 覆盖占位行（对齐服务端时间戳 / ID），callback 失败则用 snapshot 回滚。CreateTodo 额外用 `nextPendingTodoId_ = UINT64_MAX` 递减作为占位 ID（服务端真实 ID 小，不冲突）
  - **读操作（ListTodos × 2）→ 悲观 Loading**：`todosLoading_ = true` 触发 DrawTodoList 显示 `Loading...` 占位（**仅当 `todos_` 为空**，后续 refresh 不闪屏只显示底部状态），callback 回来 flip 回 false。不做 optimistic 因为"读"没有可乐观的本地状态
  - **DeleteSticky → 悲观 + 按钮禁用**：`stickyDeleting_ = true` 禁用 trashButton，HTTP 异步飞；成功直接 `PostMessageW(WM_STICKYTODO_STICKY_DELETED)` 走正常关窗路径（WS 广播兜底），失败 flip 回 false 让用户重试。**不能乐观**，因为"关窗"本身就是最终操作，关了就没有 UI 表达错误的地方
- **SettingsWindow 的 inFlight 字段**：`testInFlight_` / `loginInFlight_` / `auditInFlight_` 三个 bool。按钮 `enabled = !xxxInFlight_`，callback 无论成败都 flip 回 false。用于防止用户在请求期间狂点重复发起——比 macOS 侧的 `isLoading` 语义等价，但没有 `@Published` 的 binding 机制，手动 `InvalidateRect` 触发重绘

与 macOS 的**必须对齐的不变量**（回归测试时优先看这几条）：

1. **乐观追加**：`DraftTodoRow` 提交成功后 `todos_.push_back(new_todo)`，立即重绘，不等 WS → 与 macOS `TodoListViewModel.commitDraft` 语义一致。Windows 侧具体路径：`CommitDraft` 立即把占位 Todo 推入 `todos_`（占位 ID = `nextPendingTodoId_--` 从 `UINT64_MAX` 递减），然后发 `AsyncCreateTodo`；callback 成功用服务端 Todo 替换占位行，失败则移除占位行
2. **乐观删除**：`StickyWindow::DoDelete(rowIndex)` 先弹三选一确认框（受 `ShouldSkipTodoDeleteConfirm` 短路），用户确认后**立即**把 `todos_[rowIndex].deleted_at = "pending"`（本地软删视觉占位，UI 立刻显示恢复按钮），然后发 `AsyncDeleteTodo(todoId)`；失败则用 `prevDeletedAt` 快照回滚（大多数场景是清空 `deleted_at` 恢复未删状态）。WS `todo.deleted` 到达后 refetch 对齐真正的服务端时间戳
3. **删除确认 "N/Y/Cancel" 三选一**：`IDYES = 直接删` / `IDNO = 删除并不再提示（写 HKCU）` / `IDCANCEL = 放弃`——与 macOS 的 3-way `alert` 三按钮形态对齐（macOS 的 `@AppStorage` ↔ Windows 的 `Preferences` 封装）
4. **Frame 不跨端**：`UpsertSticky` 请求体里 `frame` 字段恒 `"{}"`，frame 只走 `FrameStore` 本机持久化

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

所有脚本都**设计为可独立跑**，CI 对前 4 个脚本直接复用（`package-docker.sh` 是**本地开发专用**，CI 的 Docker 构建另走 `docker/build-push-action@v6` + buildx，不调用本脚本）：

| 脚本 | 产出 | 依赖 | 读 `VERSION` | CI 复用 |
|---|---|---|---|---|
| `package-web.sh` | `client/web/dist/` + 同步到 `server/internal/webui/dist/` | Node.js、npm | ❌ 不读，固定构建静态产物 | ✅ `build-web` job |
| `package-server.sh` | `dist/server/stickytodo-server-<ver>-<os>-<arch>[.exe]` × 7 + 汇总 `SHA256SUMS` | Go、跑过 `package-web.sh` | ✅ 默认 `dev`，通过 `-ldflags -X main.version=` 注入到 `/health` | ✅ `build-server` job |
| `package-mac-client.sh` | `dist/mac-client/stickytodo-<ver>-macos-universal.dmg`（**或** `--skip-dmg` 时 fallback 成 `stickytodo-<ver>-macos-universal.app.zip`）+ 汇总 `SHA256SUMS`；**注意**发布文件名带版本，但 **DMG / zip 内部的 `.app` bundle 恒为 `stickytodo.app`**（不带版本），这是用户拖到 `/Applications` 后在 Launchpad / Dock 里看到的名字，必须是干净品牌名。DMG 卷标（双击 DMG 后 Finder 窗口标题）为 `stickytodo <ver>` | Xcode **26.4.x**（完整 IDE）；CI 用 `runs-on: macos-26`（**不是** `macos-latest`——后者 YAML label 目前指向 macos-15-arm64，上面根本没装 Xcode 26.x）+ `maxim-lobanov/setup-xcode@v1` **显式锁 Xcode 到 `26.4.1`**（与本机 26.4 同 `macosx26.4` SDK）。详见 §5.2 与 §7.7；DMG 打包优先 `create-dmg`（`brew install create-dmg`），缺失时 fallback 到系统自带 `hdiutil` | ✅ 默认 `dev`，**仅用于发布文件名 + DMG 卷标**，不改 App 内的 `CFBundleShortVersionString`，也不改 `.app` bundle 文件名 | ✅ `build-mac-dmg` job |
| `package-win-client.sh` | `dist/win-client/stickytodo-<ver>-windows-<arch>.zip`（portable 便携包，`<arch>` ∈ `{x64, arm64}`）+ **可选** `stickytodo-setup-<ver>-<arch>.exe`（Inno Setup 安装器；脚本检测不到 `iscc.exe` 或传 `--skip-installer` 时跳过，**不致命**）+ 每架构一份 `SHA256SUMS-<arch>`。portable zip 内顶层是 `stickytodo-<ver>-windows-<arch>/` 文件夹，内含 `stickytodo.exe`（MSVC 默认 dynamic CRT，无需额外打包 VC++ 运行时 — windows-2022 runner 和 Windows 10 19041+ 用户系统自带 VCRUNTIME140.dll；arm64 版的 arm64 CRT 同理随系统）+ 可选 `README.md` + 可选 `LICENSE.txt`（从仓库根 `README.md` / `LICENSE` 复制，缺则忽略）。setup.exe 是 per-user 安装器（默认 `{autopf}\StickyTodo`，per-user 模式下解析为 `%LocalAppData%\Programs\StickyTodo`；可选桌面快捷方式、默认不勾）。与 mac 同理，**文件名带 `<ver>` 和 `<arch>`，但 zip 内部可执行文件恒为 `stickytodo.exe`**（干净品牌名）。**架构靠 `ARCH` 环境变量选择**（默认 `x64`）：`ARCH=x64` 走 `release` preset + `x64-windows` triplet + `ilammy/msvc-dev-cmd arch=amd64`，`ARCH=arm64` 走 `release-arm64` preset + `arm64-windows` triplet + `ilammy/msvc-dev-cmd arch=amd64_arm64`（x64 host → arm64 target 交叉编译）。**arm64 自动跳过 ctest**（x64 runner 跑不了 arm64 二进制，codec/models 纯 C++ 逻辑与架构无关，x64 leg 的测试结果已覆盖） | Windows（CI: `windows-2022`）、MSVC 构建工具链（CI 镜像自带，arm64 需 `amd64_arm64` 交叉工具集，镜像自带）、vcpkg（CI 通过 `VCPKG_ROOT` 环境变量发现，镜像已预装；`arm64-windows` triplet 会触发 cppwinrt / nlohmann-json 的 arm64 重新构建，首次约 3-4 min）、可选 Inno Setup 6（CI 通过 `choco install innosetup` 按需安装）。脚本通过 `cmake --preset release` / `release-arm64` 配置，**不**依赖 `package-web.sh` | ✅ 默认 `dev`，**只用于产物文件名**；当前 `app.rc` 里的 `FILEVERSION` / `PRODUCTVERSION` **硬编码为 `1,0,0,0`**，`StringFileInfo` 里的 `FileVersion` / `ProductVersion` 字符串也硬编码为 `"1.0.0.0"`（见 `client/win/src/res/app.rc` 的 `#define VER_MAJOR/MINOR/PATCH/BUILD` 块）。如果未来要把 `$VERSION` 真正写进 PE 资源，需要改 `app.rc` 为 CMake `configure_file` 模板 + 在 `CMakeLists.txt` 解析 `APP_VERSION` 字符串拆成 4 段数字。当前未实现——与 mac 客户端 `MARKETING_VERSION=1.0` 的现状（见 §7.6）一致 | ✅ `build-win-client` job（`strategy.matrix.arch: [x64, arm64]` 并行两份） |
| `package-docker.sh` | 本地 Docker 镜像（当前平台单架构，不跨平台，`docker build` 而非 `docker buildx build`）| Docker daemon | ✅ 默认 `dev`，也作为镜像 tag | ❌ **CI 不调用**，CI 用 buildx 直推多架构 manifest |

脚本之间的依赖关系：

- 仅 `package-server.sh` 和 `package-docker.sh` 依赖 `package-web.sh` 的产物——它们把 server 编进二进制 / 镜像时会把 `server/internal/webui/dist/` 一并 `go:embed` 进去
- `package-mac-client.sh` **不依赖** web（macOS 是原生 Swift 客户端，不嵌 Web UI）
- `package-win-client.sh` **不依赖** web 同理（Windows 是原生 Win32 + Direct2D 客户端，不嵌 Web UI）。它有自己的依赖链：CMake 配置阶段 vcpkg 会拉取 `nlohmann-json`（运行期，header-only JSON 库）+ `cppwinrt`（**当前代码未使用**——保留为将来接入 Windows Toast Notifications 等 WinRT API 的预留，不占 exe 体积，详见 §7.8）以及 `gtest`（仅 `debug` preset 通过 `VCPKG_MANIFEST_FEATURES=tests` 激活 `vcpkg.json` 的 `features.tests.dependencies`，release 构建不会拉 gtest）；构建阶段需要的 Windows SDK 库在 `CMakeLists.txt` 里以 `target_link_libraries` 形式声明，当前列表：`d2d1` / `dwrite` / `dxgi` / `windowscodecs` / `credui` / `advapi32` / `shell32` / `ws2_32` / `ole32` / `uuid` / `windowsapp`。`winhttp.lib` **没有**走 CMake `target_link_libraries`，而是在 `WebSocketClient.cpp` 和 `HttpClient.cpp` 两个文件**各自**用 `#pragma comment(lib, "winhttp.lib")` 就地声明——增删系统库时要两处一起 grep（`target_link_libraries` + `grep -rn "#pragma comment(lib" client/win/src`），详见 §7.8
- `go build` 本身在 dist 缺失时也能通过（webui.go 会回退到内置的 placeholder 页）

另外 `package-web.sh` 的 npm 安装策略是：检测到 `package-lock.json` 走 `npm ci`（可复现）；没有 lockfile 才降级为 `npm install`。所以手动在 `client/web/` 下跑 `npm install` 是开发便利写法，CI / 打包脚本走的是 `npm ci`。

### 5.2 GitHub Actions

三个 workflow 文件分工：

- **`_build-all.yml`**（`on: workflow_call`）：reusable workflow，输入 `version` / `tag_name` / `prerelease` / `docker_image` / `tag_latest`，包含 7 个 job：
  1. `build-web`
  2. `build-server`（矩阵：linux × amd64/arm64/armv7、darwin × amd64/arm64、windows × amd64/arm64）
  3. `build-mac-dmg`（`runs-on: macos-26` runner——**明确不是** `macos-latest`，因为 runner-images 主 README 明载 `macos-latest` YAML label 当前指向 `macos-15-arm64`（macOS 15.7.x + Xcode 16.x 系列），那上面根本没有 macOS 26 SDK，和本机 Xcode 26.4 无法对齐；而 `macos-26` 是 macOS 26 Tahoe arm64 runner，`xcode-select -p` 默认指 `Xcode_26.2.app`、但预装列表里还有 26.5 beta / 26.4.1 / 26.3 / 26.1.1 / 26.0.1。`+ maxim-lobanov/setup-xcode@v1 xcode-version: '26.4.1'` 显式切到 `macosx26.4` SDK（字面量 `26.4.1` 在预装列表里精确匹配 `/Applications/Xcode_26.4.1.app`，其 symlink `Xcode_26.4.app` 同样存在；setup-xcode 其实支持 SemVer，写 `26.4` 也能解析到同一目标，字面量只是零歧义） + `brew install create-dmg || true` + `package-mac-client.sh`）。**为什么必须锁 Xcode 而非依赖 macos-26 runner 的默认 26.2**：macOS 26.2 SDK 和本机的 macOS 26.4 SDK 在 SwiftUI `.buttonStyle(.bordered)` 的默认填充基色上不等价——便签加号按钮在 macosx26.2 SDK 下编译出来是**白底**、在 macosx26.4 SDK 下是**灰底**。源码层在 `StickyView.swift` 里加的 `.tint(.secondary)` + `.controlSize(.small)` 是第二道防线（当未来锁定版本因镜像轮转失效时兜底），CI 侧锁 SDK 才是根本解。详见 §7.7
  4. `build-win-client`（`runs-on: windows-2022` runner——**明确不是** `windows-latest`，与 §5.2 mac 同理：保持 runner 镜像固定，避免 MSBuild / Windows SDK 版本随 `-latest` 静默漂移导致产物行为差异。镜像自带 MSVC 2022、CMake ≥ 3.25、Ninja、vcpkg（预装路径由镜像暴露的 `VCPKG_INSTALLATION_ROOT` 提供）。**该 job 启用 `strategy.matrix.arch: [x64, arm64]` 并行两份**（`fail-fast: false` 保证两个 leg 都跑完，maintainers 能同时看到 x64 / arm64 的失败），每个 leg 的 steps 序列：①`actions/checkout@v4` ②`Activate MSVC dev environment (${{ matrix.arch }})` —— `ilammy/msvc-dev-cmd@v1`，`arch` 参数用表达式 `matrix.arch == 'arm64' && 'amd64_arm64' || 'amd64'`：x64 leg 走 `amd64`（native x64-host x64-target），arm64 leg 走 `amd64_arm64`（x64-host + arm64-target 交叉编译，cl.exe 仍在 x64 runner 上运行、只是换了 arm64 codegen）③`Install Inno Setup (if missing)` —— pwsh 里先 `Test-Path 'C:\Program Files (x86)\Inno Setup 6\ISCC.exe'`，不存在才 `choco install innosetup --no-progress -y`（幂等，windows-2022 镜像通常预装；`choco install` 的"已安装则 no-op"保证无副作用） ④`Export VCPKG_ROOT` —— bash step 把 `VCPKG_INSTALLATION_ROOT` 映射到 `VCPKG_ROOT` 写进 `GITHUB_ENV`，后续 steps 和 `CMakePresets.json` 的 `$env{VCPKG_ROOT}` 才能解析 ⑤`Package Windows client (${{ matrix.arch }})` —— `VERSION: ${{ inputs.version }}` + `ARCH: ${{ matrix.arch }}` + `bash scripts/package-win-client.sh`（脚本按 ARCH 选 CMake preset、vcpkg triplet、iscc `/DAppArch` define、产物命名） ⑥`Upload Windows artifacts (${{ matrix.arch }})` —— `actions/upload-artifact@v4` 上传 `dist/win-client/stickytodo-*-windows-${{ matrix.arch }}.zip` + `dist/win-client/stickytodo-setup-*-${{ matrix.arch }}.exe` + `dist/win-client/SHA256SUMS-${{ matrix.arch }}`，artifact name = `win-client-${{ matrix.arch }}`（两 leg 名字不同、绝不会互相覆盖），`if-no-files-found: error` 保证产物缺失时直接失败。该 job 有意**不**写 `needs: build-web`——Windows 客户端不嵌 Web UI，与 `build-web` 并行启动省墙钟时间。**为什么 arm64 在 x64 runner 上交叉编译而不是用原生 arm64 runner**：GitHub Actions 截至本节写就时尚未提供公开的 Windows arm64 runner，`amd64_arm64` 交叉工具集是唯一可行方案；等未来 `windows-2022-arm64` 或类似 label 就绪，可把 arm64 leg 改成原生 runner + `arch: arm64`，删掉交叉编译路径）
  5. `detect-docker-creds`（只有 3 行：读 `secrets.DOCKERHUB_USERNAME` 是否非空，输出 `have=true/false`；存在是因为 `secrets.*` 不能直接用在 `if:` 表达式里）
  6. `build-docker`（`needs: [build-web, detect-docker-creds]`、`if: needs.detect-docker-creds.outputs.have == 'true'`，用 `docker/setup-qemu-action` + `docker/setup-buildx-action` 推 `linux/amd64,linux/arm64,linux/arm/v7` 多架构）
  7. `publish-release`（`needs: [build-server, build-mac-dmg, build-win-client, build-docker]`；条件是多行 `if:` 表达式，容忍 `build-docker` 在没 secrets 时被 `skipped`，但 server / mac / win 任一失败仍会中止——**矩阵 job 的 `.result` 是所有 leg 的聚合**，所以 x64 / arm64 任一失败都会让 `build-win-client.result` 变成 `failure` 从而阻塞 release；用 `softprops/action-gh-release@v2` 把 `build-server` / `build-mac-dmg` / `build-win-client`（两次 download：`win-client-x64` + `win-client-arm64`，都下到同一个 `dist/win-client/` 目录；arch 后缀保证 zip / setup.exe / SHA256SUMS 三类文件不冲突）的 artifact 挂到 Release）
- **`release-tag.yml`**（`on: push: tags: ['v*']`）：调用 `_build-all.yml`，`docker_image=docker.io/hanxi/stickytodo`、`tag_latest=true`、正式发布
- **`release-branch.yml`**（`on: workflow_dispatch`，带 `branch` 输入）：先跑 `cleanup-old-release` job，**三阶段兜底**删同名旧 release（①`gh release delete --cleanup-tag` → ②降级为 `gh release delete` + `git push --delete origin <tag>` → ③容忍 tag/release 都不存在的首次运行），再调 `_build-all.yml` 生成 prerelease，`tag_latest=false` 确保不会覆盖 `:latest` 镜像

所需 secrets：`DOCKERHUB_USERNAME` / `DOCKERHUB_TOKEN`。**不是"不配就跳过 push"，而是"不配就完全跳过 `build-docker` 这个 job"**（镜像不会构建、不会推送）；其他产物不受影响。完整手册见 [docs/RELEASE.md](./docs/RELEASE.md)。

### 5.3 产物矩阵

| 产物类型 | 命名 | 备注 |
|---|---|---|
| Server 二进制 | `stickytodo-server-<ver>-<os>-<arch>[.exe]` | 7 份：linux × (amd64/arm64/armv7)、darwin × (amd64/arm64)、windows × (amd64/arm64)；与同目录的 `SHA256SUMS` 汇总文件一起上传 |
| Mac 客户端 | 发布文件名：`stickytodo-<ver>-macos-universal.dmg`；DMG 卷标（挂载后 Finder 窗口标题）：`stickytodo <ver>`；DMG 内 `.app` bundle：**`stickytodo.app`**（无版本号） | universal（arm64 + x86_64）；脚本用 `codesign --force --deep --options runtime --sign -` 做 **ad-hoc** 签名（`--sign -` 等价短写 `-s -`），`--options runtime` 启用 Hardened Runtime 以便将来可平滑切到开发者 ID 签名；同目录一份 `SHA256SUMS`。**三套命名的分工**：① 发布文件名要带 `<ver>` 供下载归档区分；② DMG 卷标要带 `<ver>` 方便用户知道自己挂的是哪个版本；③ `.app` bundle 名必须是干净的 `stickytodo.app`——这是用户拖进 `/Applications` 后在 Launchpad / Dock / Cmd+Tab 里永久看到的名字，绝不能混入发布文件名里的 `branch-main` / `v1.2.3` 之类噪声。历史 bug：曾把这三套名字合成一套，导致 DMG 双击开打是 "stickytodo branch-main (2849)" 这种脏窗口标题，且拖进 /Applications 后 App 图标下方显示 `stickytodo-branch-main-macos-universal`，非常不像正式应用 |
| Win 客户端（portable） | 发布文件名：`stickytodo-<ver>-windows-x64.zip` + `stickytodo-<ver>-windows-arm64.zip`（**无 `-portable` 后缀**，脚本里直接用干净命名）；zip 内顶层是 `stickytodo-<ver>-windows-<arch>/` 文件夹 → 内含 `stickytodo.exe`（无版本号——同 mac `.app` 的干净品牌名原则）+ 可选 `README.md` + 可选 `LICENSE.txt` | **双架构：x64（amd64）+ arm64**，两份独立产物，用户按自己系统下载对应包（Win11 arm64 系统即便能跑 x64 emulation 也推荐原生 arm64 版：更省电、性能更好）；**无代码签名**（首次启动会触发 SmartScreen 警告，用户需点击"更多信息 → 仍要运行"，这是开源项目可接受的 onboarding cost）；每架构一份 `SHA256SUMS-x64` / `SHA256SUMS-arm64` |
| Win 客户端（installer） | 发布文件名：`stickytodo-setup-<ver>-x64.exe` + `stickytodo-setup-<ver>-arm64.exe`（**`setup-<ver>-<arch>` 顺序**，Inno Setup 6 编译） | per-user 安装（默认不需 UAC；`DefaultDirName={autopf}\StickyTodo` + `PrivilegesRequired=lowest` + `PrivilegesRequiredOverridesAllowed=dialog commandline`，允许用户在安装向导里显式升级成 all-users 模式，此时 `{autopf}` 解析成 `{commonpf}`=`%ProgramFiles%\StickyTodo`）；`MinVersion=10.0.19041` 把可安装系统卡在 Windows 10 20H1+；**架构门禁**：x64 安装器 `ArchitecturesAllowed=x64compatible`（允许 native x64 + Win11-arm64 上的 x64 emulation 作为 fallback），arm64 安装器 `ArchitecturesAllowed=arm64`（**仅允许真实 arm64 host**，不接受 emulation，保证 arm64 二进制只落在真能原生运行它的系统上）；可选创建桌面快捷方式（Task `desktopicon` 带 `Flags: unchecked`，安装向导里**默认不勾**）；卸载器通过"设置 → 应用 → 已安装的应用"找到，arm64 版的 `UninstallDisplayName` 是 `StickyTodo (arm64)` 便于用户区分当前装的是哪个架构（x64 版保持 `StickyTodo` 无后缀）；`AppId` 是固定 GUID `{{4B5B6C2E-9E7B-4F3D-A8C5-0D6A1B2C3D4E}}` **两架构共用**（frozen，绝不变）——故意设计成共享升级通道：同一主机上若先装 x64 再装 arm64（或反向）会自动替换，而不是并存两份（单台机器不存在两架构都需要的合理场景）；每架构一份 `SHA256SUMS-<arch>` 与 portable zip 合用 |
| Docker 镜像 | `docker.io/hanxi/stickytodo:<ver>`（正式 tag 时还会打 `:latest`）| 多架构 manifest：`linux/amd64` / `linux/arm64` / `linux/arm/v7`；镜像分发**不带** SHA256SUMS，完整性靠 registry digest |

`SHA256SUMS` 由 `package-server.sh` / `package-mac-client.sh` / `package-win-client.sh` 在结尾处统一生成：优先 `sha256sum`（Linux、windows-2022 Git Bash 自带），缺失时 fallback `shasum -a 256`（macOS 自带），产物文件名以相对路径写入同目录的 `SHA256SUMS`。Docker 镜像没有也**不应该**有这个文件——镜像完整性靠 registry 返回的 content digest（`sha256:...`）校验。

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

改 Windows 客户端时额外跑（**Windows 环境下，Git Bash / MSYS2**；脚本检测到 `VCPKG_ROOT` 环境变量才能配置 CMake）：

```bash
# 走 debug preset（自带 BUILD_TESTS=ON + vcpkg features=tests），
# configure + build + ctest 一把梭；失败时 ctest 会按用例打印日志。
cd client/win
cmake --preset debug
cmake --build --preset debug --config Debug
( cd build/debug && ctest --output-on-failure -C Debug )
```

或者跑完整打包流程（会额外产 portable zip；传 `--skip-installer` 跳过 Inno Setup）：

```bash
# 从仓库根执行
VERSION=dev bash scripts/package-win-client.sh --skip-installer
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

### 7.1 四端字段必须同步

后端 `server/internal/model/models.go` 里每个字段的改动（新增、改 JSON tag、改类型），**必须**在以下三处同步修改：

- `client/web/src/types/api.ts`
- `client/mac/stickytodo/Models/*.swift`
- `client/win/src/models/*.h` + `client/win/src/codec/JsonHelper.cpp`（Windows 客户端用 `nlohmann::json` 手写 parse/serialize，**不是**自动 codegen）

否则三端都会**抛错**（不是静默失败）：

- TypeScript：`any` 宽容类型不会报，但运行时读 `undefined.xxx` 会炸
- Swift：`JSONDecoder.decode(...)` 遇到类型不匹配会抛 `DecodingError`，APIClient 会把它包成 `APIError.decoding(...)` 返回给 UI（见 `Networking/APIClient.swift`），用户可见但不会崩溃
- C++：`JsonHelper` 里用 `j.value("key", default)` 或 `j.contains("key") && j["key"].is_xxx()` 做防御，读不到就取 default，**不会抛**——但对应字段在 UI 上会变成默认值（如空串 / 0），表现为数据"丢失"。所以加字段时**必须**在 `Parse*` 和 `*ToJson` 两个方向都补上

改 DTO 后三端都要自测：
- Web：`npm run typecheck` + 跑一下相关页面
- macOS：Xcode build（`JSONDecoder` 的 error 是 compile-time 查不出的，只能运行时）
- Windows：跑 `client/win/tests/` 下的 gtest，共两个测试可执行文件：`test_models_json`（覆盖 Todo / StickyNote / AuditLog / Filter 的 `Parse*` / `*ToJson` 往返）+ `test_sticky_codec`（覆盖 `StickyCodec` 的 `bg_color` / `filter` 字段序列化与反序列化）——加字段时务必同时更新对应 test 的 fixture，否则 `ctest --output-on-failure -C Debug` 会报差异

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
- Windows 客户端**同时打 x64（amd64）+ arm64**，两份独立产物。`CMakePresets.json` 里两架构各有一对 configure preset：`release` + `debug`（x64，绑 `x64-windows` triplet）/ `release-arm64` + `debug-arm64`（arm64，绑 `arm64-windows` triplet）。CI `build-win-client` job 用 `strategy.matrix.arch: [x64, arm64]` 并行两份，`ilammy/msvc-dev-cmd@v1` 的 arch 参数表达式 `matrix.arch == 'arm64' && 'amd64_arm64' || 'amd64'` 切换交叉工具集。**arm64 是 x64 runner 上的交叉编译**——binary 能构建、但不能在 x64 host 上执行，所以 `package-win-client.sh` 在 `ARCH=arm64` 时**无条件跳过 ctest**（codec / models 测试是纯 C++ 逻辑，x64 leg 已覆盖）。产物命名 `stickytodo-<ver>-windows-<x64|arm64>.zip` + `stickytodo-setup-<ver>-<x64|arm64>.exe`，**不能**把两架构塞进同一 installer 混合分发——Inno Setup 的 `ArchitecturesAllowed` 按架构 gating（x64 用 `x64compatible`、arm64 用 `arm64`），AppId GUID 共用以维持单一升级通道。未来若 GitHub Actions 提供原生 Windows arm64 runner（如 `windows-2022-arm64`），可把 arm64 leg 迁到原生 runner + `arch: arm64`，ctest 就能跑起来，删掉 `package-win-client.sh` 里 "arm64 跳过 ctest" 的分支
- Windows 客户端**严禁**引入需要 MinGW 工具链的依赖——CI 和本机都走 MSVC（MSBuild / cl.exe），vcpkg 默认 triplet 是 `x64-windows`（dynamic CRT）。如果某个库只在 `x64-mingw-dynamic` 有，第一反应应是找 MSVC 替代，而不是切 triplet（会连锁触发所有其它 vcpkg 依赖的重建）

### 7.6 版本号来源

- CI 里版本来自 `github.ref_name`（tag 名）
- 本地脚本来自 `$VERSION` 环境变量，`package-server.sh` / `package-mac-client.sh` / `package-docker.sh` 均默认 `dev`；`package-web.sh` 不读 `VERSION`（静态产物）
- 后端二进制启动时 `/health` 返回的 `version` 由 `-ldflags "-X main.version=..."` 在 build 时注入，用户能实时看到
- **Mac 客户端版本号的限制**：当前 `MARKETING_VERSION` 在 `stickytodo.xcodeproj/project.pbxproj` 里**硬编码为 `1.0`**，`package-mac-client.sh` 不会修改 Info.plist，因此 DMG 里的 App "关于"信息永远显示 `1.0`；外部可见的版本号只有**产物文件名**（`stickytodo-<VERSION>-macos-universal.dmg`）。如果未来需要把 `$VERSION` 真正写进 App Bundle，需要在 `package-mac-client.sh` 的 xcodebuild 阶段额外改 pbxproj 的 `MARKETING_VERSION` 或用 `PlistBuddy` 改生成后的 `*.app/Contents/Info.plist`

### 7.7 macOS 客户端 Xcode / SDK 版本一致性

**当前策略（双保险）**：

1. **CI 侧锁 runner + 锁 Xcode 版本**：`_build-all.yml` 的 `build-mac-dmg` job
   - `runs-on: macos-26`（**不是** `macos-latest`）
   - `maxim-lobanov/setup-xcode@v1` + `xcode-version: '26.4.1'`，使用 `macosx26.4` SDK
2. **源码侧显式 modifier**：`Views/StickyView.swift` 的便签加号按钮（`titleBar` 内）追加 `.tint(.secondary)` + `.controlSize(.small)`——作为 SDK 漂移的第二道防线，即使未来不得不临时降级到 26.3 SDK 时仍能保住基本外观

**关键事实（决策前请先读这段；别再凭印象猜了）**：

- **`macos-latest` 不是"最新 macOS"**。runner-images 主 README（`https://github.com/actions/runner-images` 的 "Available Images" 表）当前明载：
  - macOS 15 Arm64 → YAML label `macos-latest`, `macos-15`, `macos-15-xlarge`
  - macOS 26 Arm64 → YAML label `macos-26`, `macos-26-xlarge`（**和 `macos-latest` 是两个不同的镜像，互不相关**）
  
  GitHub Actions 的 `-latest` label 迁移非常保守，2025-08 才从 macos-14 迁到 macos-15，macOS 26 目前不会被自动收编为 `-latest`。想用 macOS 26 SDK 就必须显式写 `runs-on: macos-26`
- **`macos-26` runner 的 Xcode 预装列表**（截至本节写就时，来自 `images/macos/macos-26-arm64-Readme.md` 的 `### Xcode` 表；如果 runner 镜像升级了，请以 readme 为准）：
  - 26.5 (beta) → `/Applications/Xcode_26.5_beta_2.app`（symlinks: `Xcode_26.5.0.app` / `Xcode_26.5.app`）
  - **26.4.1** → `/Applications/Xcode_26.4.1.app`（symlink: **`Xcode_26.4.app`**，即裸 `26.4` 也是合法字面量）
  - 26.3 → `/Applications/Xcode_26.3.app`
  - **26.2 (default)** → `/Applications/Xcode.app`（`xcode-select -p` 默认指这里，但**这是 macos-26 runner 的默认，不是 macos-latest 的**）
  - 26.1.1、26.0.1 等
- **setup-xcode@v1 的版本匹配规则**：官方 README（`https://github.com/maxim-lobanov/setup-xcode`）明载支持 **SemVer**，例如 `16`、`16.4`、`26.3`、`^16.2.0` 都合法；不是"必须精确字面量"。但字面量（如 `26.4.1`）在 workflow yaml 里更醒目、升级时一眼能看出改了哪一版，所以仍推荐字面量
- 维护者本机 Xcode 26.4（macOS 26.4 Tahoe），对应 SDK `macosx26.4`——这是和 `runs-on: macos-26` 上 `Xcode_26.4.1.app` 一致的目标 SDK

**决策历史**（每一步都要写清"为什么上一步不够"，避免后人重复踩坑）：

1. **阶段 1（`macos-14` + Xcode 15.4）**：CI 和本机 Xcode 跨主版本，SwiftUI `.buttonStyle(.bordered)` 默认外观填充差异明显——便签加号按钮在 CI DMG 里是**白底**、本机 Debug 是**浅灰底**。典型跨 SDK 视觉漂移
2. **阶段 2（尝试：`runs-on: macos-latest` + `setup-xcode 26.4`）**：**本阶段基于多个错误前提**：
   - 错 ①：以为 `macos-latest` 指向 macos-26 runner（实际指向 macos-15-arm64，上面完全没有 Xcode 26.x）
   - 错 ②：以为 setup-xcode "必须精确字面量匹配"（实际支持 SemVer）
   
   失败日志：`Could not find Xcode version that satisfied version spec: '26.4'`。**真正原因**是 macos-15-arm64 runner 上就没装任何 Xcode 26.x，setup-xcode 在 `/Applications/` 下找不到任何能满足 `26.4` 的 app bundle——和字面量/SemVer 无关
3. **阶段 3（尝试：`runs-on: macos-latest` + 不锁 Xcode + 只靠源码层 `.tint(.secondary)` + `.controlSize(.small)`）**：误把问题归因到"runner 默认 Xcode 和本机只差一点"，想靠源码层兜底。实测 CI 产出的加号仍是白底、本机灰底。**真正原因**依然是 `macos-latest` = macos-15 runner 根本没有 macOS 26 SDK，产物其实是 macOS 15 SDK（Xcode 16.x）编出来的，和本机 macOS 26.4 SDK 跨了整整一个主版本——源码层 modifier 覆盖不住这么大的 SDK 差
4. **阶段 4（当前：`runs-on: macos-26` + `setup-xcode 26.4.1` + 保留源码层 modifier）**：真正把 runner 从 macos-15 切到 macos-26（这才有 macOS 26 SDK 可选），再锁 Xcode 26.4.1 对齐本机。锁 runner + 锁 Xcode = 第一道防线（SDK 对齐），源码层 modifier = 第二道防线

**如何升级 / 调整锁定的 Xcode 版本**：

1. 打开 `https://github.com/actions/runner-images/blob/main/images/macos/macos-26-arm64-Readme.md`（如果目标是 macos-27 GA，就去 `macos-27-arm64-Readme.md`），找到 `### Xcode` 的表格
2. 从表格挑一个**字面量**——可以是主版本号（`26.5`）或完整修订号（`26.4.1`）；由于 setup-xcode 支持 SemVer，如果想写 `26.4` 也合法（会解析到 `Xcode_26.4.1.app` 的 symlink）。不过推荐字面量完整版本号（`26.4.1`）以便一眼看出和本机对齐的是哪个修订
3. 同时改 3 处（全部改完才算一次升级完整）：
   - `.github/workflows/_build-all.yml` 的 `- name: Select Xcode ...` step 的 `xcode-version` 字面量
   - `.github/workflows/_build-all.yml` 的 `build-mac-dmg` 注释块里"预装列表"的版本号快照
   - 本文档 §5.1 表格的 `package-mac-client.sh` 那行、§5.2 `build-mac-dmg` 描述、本节（§7.7）的 "当前策略" + "关键事实" 两处版本号
4. 如果 runner 镜像本身要升（比如 macOS 27 GA 后想迁到 macos-27），那还要把 `runs-on` 同步改，且 §7.7 的"关键事实"段落里的 runner-images README 引用也要改

**仍然要守的纪律**：

1. **`runs-on` 必须显式写 `macos-26`，绝不能改回 `macos-latest`**。已经踩过两次坑，`-latest` 不等于"最新 macOS"。即使未来 `macos-latest` 有一天迁到 macos-26 了，也要等迁移稳定、且本文档确认过之后才能改
2. **CI 侧的 `setup-xcode` step 不能删**。只依赖 runner 默认 Xcode（`macos-26` 默认是 26.2）会与本机 26.4 跨两个次要版本，`.buttonStyle(.bordered)` 的填充基色会漂
3. **源码层的显式 modifier 也不能删**。当前至少 `StickyView.swift` 的加号按钮依赖 `.tint(.secondary)` + `.controlSize(.small)`。未来如果发现 `MenuBarContent.swift` / `SettingsView.swift` 里的 `.bordered` / `.borderedProminent` / `.menuStyle(.borderlessButton)` 在 CI 产物和本机有分歧，第一反应应是**再给它们也加显式外观 modifier**
4. **本机 Xcode 不要比 CI 锁的版本超前太多**。当前约束：本机 Xcode 主次版本 ≤ 锁定版本的主次版本 + 1。例如锁 26.4.1，本机用 26.4 / 26.5 都可接受；本机升到 27.x 就必须先把 CI 锁定版同步升级（前提：runner 预装列表里有对应字面量），否则 CI 和本机再次跨 SDK 漂移
5. **runner 镜像更新后的抽检**：runner-images 大约每 2-3 周 release 一次，有时会在 minor release 里轮转掉老 Xcode。建议每月手动跑一次 `release-branch` workflow，挂载产出的 DMG、肉眼看一眼加号 / 其它关键按钮的外观；如果发现 setup-xcode step 开始报 `Could not find Xcode version` 错误，立刻到 runner-images readme 里查最新预装列表，按上面"如何升级"的流程更新

### 7.8 Windows 客户端 vcpkg / WinHTTP / Inno Setup 纪律

- **vcpkg manifest 模式**：依赖全写在 `client/win/vcpkg.json`，**不要**用 classic 模式的 `vcpkg install xxx`（会污染全局）。当前 top-level `dependencies` 有两个：①`nlohmann-json`（运行期 header-only JSON 库，被 `JsonHelper` / `StickyCodec` 使用）②`cppwinrt`（**当前代码尚未使用**——grep `client/win/src` 没有任何 `#include <winrt/...>` 或 `cppwinrt.exe` 自定义构建规则。保留它和 `CMakeLists.txt` 里的 `windowsapp` import lib 是为**未来引入 C++/WinRT API 做预留**——比如将来接入 Windows Toast Notifications / ApplicationData 时可以直接 `#include <winrt/Windows.UI.Notifications.h>` 而无需改 `vcpkg.json`。不增加 exe 体积 / 启动开销 — `cppwinrt` header 全是 header-only，没被 `#include` 就不产生代码；`windowsapp.lib` 是 umbrella import lib，链接器对未引用符号不做任何处理）。`features.tests.dependencies` 只有 `gtest`，由 `debug` preset 的 `VCPKG_MANIFEST_FEATURES=tests` 激活，release 构建**不**拉 gtest。加新依赖时：①改 `vcpkg.json` 的 `dependencies` 数组 ②如果是仅测试用依赖，放到 `features.tests.dependencies` 下 ③`cmake --preset` 时 vcpkg 自动拉取、无需手动 install
- **vcpkg baseline**：`vcpkg-configuration.json` 里钉死了 `default-registry.baseline` 到具体 commit `c82f74667287d3dc386bce81e44964370c91a289`。升级 baseline 时必须整体跑一次 debug 构建 + ctest，因为 vcpkg baseline 升级会同时提升**所有**依赖的版本（不是按包粒度），容易触发连锁兼容问题
- **WinHTTP WebSocket 协议纪律**（呼应 §3.3 ws 契约）：
  - `/api/ws` 契约规定"除首帧 auth 外不接受任何上行业务帧 → close 4400"。`WebSocketClient.cpp` 的 `ReceiveLoop` **绝对不要**发业务/应用层 ping（会被服务端当作违规上行帧立刻 close 4400）。保活靠服务端 30s WS ping，WinHTTP 会**自动**回 pong，无需应用层做任何事
  - `WinHttpWebSocketReceive` 是阻塞调用。想让 `Disconnect()` 能立即唤醒它的唯一可靠办法是**从另一线程关闭 handle**——这就是 `liveWebSocket_` `std::atomic<void*>` 的用途：`Disconnect` `exchange(nullptr)` 拿到 handle、调 `WinHttpCloseHandle`，receive 立即返回 `ERROR_WINHTTP_OPERATION_CANCELLED`，worker 线程检测到 `shouldRun_ == false` 直接 break。**不要**试图用 `WinHttpSetTimeouts` + 短超时轮询实现"伪阻塞"，那会退化成 busy loop
  - WS 回调（`onEvent_` / `onSignal_`）在 worker 线程执行，`AppState` 订阅的 lambda **必须**把事件 marshall 回 UI 线程（当前实现：`PostWsEventToUIThread` / `PostWsSignalToUIThread` → `WM_STICKYTODO_WS_EVENT` → `Tray::WndProc` → `AppState::HandleWsEventOnUIThread`）。新加回调时不要直接在 lambda 里访问 `stickies_` / `HWND` 等 UI 线程拥有的数据，否则触发 UB
  - **`winhttp.lib` 是全仓库唯一不在 `CMakeLists.txt` 的 `target_link_libraries` 里的系统库**：它只通过 `WebSocketClient.cpp` 和 `HttpClient.cpp` 两个文件顶部**各自**的 `#pragma comment(lib, "winhttp.lib")` 就地声明。对比而言，`CMakeLists.txt` 当前 `target_link_libraries` 里列的系统库（`d2d1` / `dwrite` / `dxgi` / `windowscodecs` / `credui` / `advapi32` / `shell32` / `ws2_32` / `ole32` / `uuid` / `windowsapp`）中，只有一部分在对应源文件里**额外**叠加了 pragma——`D2DRenderer.cpp` 叠加 `d2d1.lib` / `dwrite.lib`，`TrayIcon.cpp` 叠加 `shell32.lib` / `ole32.lib`，`CredentialStore.cpp` 叠加 `advapi32.lib`（双声明属于冗余保险，MSVC 链接器按符号去重，不冲突）；其余的 `dxgi` / `windowscodecs` / `credui` / `ws2_32` / `uuid` / `windowsapp` **只在 CMake 里单点声明**，没有 pragma 叠加。这意味着**winhttp 是唯一 pragma-only** + **少数库是 CMake+pragma 双声明** + **多数库只在 CMake 单点声明**三种口径并存，增减系统库时要同时 `grep -rn "#pragma comment(lib" client/win/src` + 对照 CMakeLists 的 `target_link_libraries`，避免漏看任一侧的声明
- **UI 线程模型**：
  - Windows 客户端只有一个 UI 线程（主线程），所有窗口过程（`StickyWindow::WndProc` / `SettingsWindow::WndProc` / `FilterEditor::WndProc` / `TrayIcon::WndProc`）都在它上面运行
  - WS worker 线程通过 `PostMessageW(uiThreadTarget_, WM_STICKYTODO_WS_EVENT, ...)` 发消息——`uiThreadTarget_` 是 tray 的隐藏消息窗口（tray 最早创建，生命周期覆盖整个 App 运行期）
  - 便签窗口间的广播通过 `AppState::PostMessageToAllStickies(WM_STICKYTODO_REFRESH)`——每个 StickyWindow 收到后调 `RefreshFromState()` 重新从 `AppState` 拉数据 + Invalidate
- **Inno Setup 脚本**（`installer/setup.iss`）：
  - `AppId` 固定 GUID `{{4B5B6C2E-9E7B-4F3D-A8C5-0D6A1B2C3D4E}}`（frozen，**两架构共用**）。**绝不要**每次发版生成新 GUID、**也不要**给 arm64 单独换 GUID——AppId 是 Windows "已安装应用"列表里判定"升级 or 并存"的主键，改了 AppId 就意味着老版本不会被自动覆盖、会并存两份；两架构共 GUID 的刻意设计让同一主机上 x64 → arm64 切换能走升级路径（用户从"x64 emulation 版"过渡到"原生 arm64 版"时，不会在 Add/Remove Programs 里留下两份僵尸条目）
  - 当前安装行为：`PrivilegesRequired=lowest` + `DefaultDirName={autopf}\StickyTodo` + `PrivilegesRequiredOverridesAllowed=dialog commandline`。组合效果：默认不弹 UAC，走 per-user 模式（`{autopf}` 解析成 `{userpf}` = `%LocalAppData%\Programs\StickyTodo`）；用户也能在安装向导里显式勾选"为所有用户安装"，此时弹 UAC，`{autopf}` 解析成 `{commonpf}` = `%ProgramFiles%\StickyTodo`。**不要**误以为 `DefaultDirName` 写死 `{localappdata}\Programs\StickyTodo` 才是 per-user——那样会丢掉 per-machine 升级路径
  - **架构 gating**（`/DAppArch=<x64|arm64>`）：x64 版 `ArchitecturesAllowed=x64compatible` + `ArchitecturesInstallIn64BitMode=x64compatible`（允许 native x64 + Win11-arm64 的 x64 emulation 作为 fallback，后者是 Microsoft 建议的 "no native arm64 yet" 兜底路径；`x64compatible` 关键字是 Inno Setup 6.3+ 引入的专用语义，比老的 `x64` 更精确）；arm64 版 `ArchitecturesAllowed=arm64` + `ArchitecturesInstallIn64BitMode=arm64`（**仅** native arm64，不接受 emulation——确保 arm64 原生二进制只落在能直接执行它的系统上，避免用户误装后触发莫名的启动失败）。`ArchitecturesInstallIn64BitMode` 还会把 `{autopf}` / 注册表视图切到 64-bit 变体，否则 64-bit exe 会错误地落在 `%ProgramFiles(x86)%`
  - `UninstallDisplayName`：x64 版 `StickyTodo`（无后缀），arm64 版 `StickyTodo (arm64)`（带括号架构标）。仅 `UninstallDisplayName` 分架构；`AppName` / Start Menu `DefaultGroupName` / 桌面快捷方式全都是裸 `StickyTodo`（匹配 Chrome / VS Code / Zoom 的做法——架构只在"已安装应用"详情页暴露，不污染日常使用 UI）
  - `MinVersion=10.0.19041` 卡住 Windows 10 20H1 为最低版本，与 DirectWrite / Direct2D 现代特性匹配；早期 Windows 10 和 Windows 8.1 会在安装时被 Inno Setup 直接拒绝
  - `Source:` 段落以 iscc 的 `/DArtifactDir=...` 参数为基准。`package-win-client.sh` 通过 `/DArtifactDir=$(to_win "$OUT_DIR/$ARTIFACT_BASE")` 把已构建好的 `dist/win-client/stickytodo-<ver>-windows-<arch>/` 目录路径传给 iscc（staging 目录名也带架构后缀，两架构并行构建不会互相覆盖），`.iss` 里写 `Source: "{#ArtifactDir}\stickytodo.exe"` 解析即生效——**不**需要先复制到 `installer/` 下做 staging。`.iss` 的 fallback `#define ArtifactDir` 也按 `AppArch` 分支（`build\release` vs `build\release-arm64`），保证在 CI 不传 `/D` 的本地 iscc 裸调用场景里也能产出正确路径
- **资源编译纪律**：
  - `client/win/src/res/app.rc` 里的 `FILEVERSION` / `PRODUCTVERSION` 必须是 4 段数字（如 `1,0,0,0`），不能写 `dev` / `1.2.3-rc1` 这种语义版本。当前通过 `#define VER_MAJOR/MINOR/PATCH/BUILD` 硬编码为 `1,0,0,0` —— **尚未**与 `$VERSION` 联动（见本节前文 Bundle 说明），未来做联动时要在 CMakeLists 里解析 `APP_VERSION` 字符串 → 拆 4 段数字 → `configure_file` 模板替换，无法解析时退化为 `0,0,0,0`
  - 图标资源：`client/win/src/res/app.rc` 引用 `icons\\stickytodo.ico`（相对 `.rc` 自身目录，即 `client/win/src/res/icons/stickytodo.ico`）。该 ico 由 `scripts/generate-icons.sh` 的 `build_windows_ico` 段落从 `assets/branding/stickytodo-icon.svg` 自动派生，包含 **16/20/24/32/40/48/64/128/256** 共 9 档帧（256 走 Vista+ 的 PNG 压缩格式嵌入），覆盖 Taskbar / Alt-Tab / Start Menu / Explorer 各 DPI 所有请求尺寸——**禁止**只 checkin 单档小图，Windows 会 bilinear 放大导致其他尺寸模糊。打包器优先级：`magick` → `convert` → `icotool` → `png2ico`（后者不支持 256 帧，会降级并 warn）；更新品牌时改完 SVG 后跑 `scripts/generate-icons.sh`（或 `--win-only`）即可，ico 仍 checkin 入库（Windows rc 编译链路需要它作为 `ICON` 资源的物理文件，不能像 macOS 那样走运行期渲染）。调用约束：`--mac-only` / `--web-only` / `--win-only` 三者互斥，同时传会 `exit 2`
  - Manifest：`client/win/src/res/app.manifest` 声明 DPI 感知、UTF-8 活动代码页、Common Controls v6，通过 `1 RT_MANIFEST "app.manifest"` 嵌入 exe。修改 manifest 不需要改 CMake（rc 引用是相对路径），但修改后必须做一次干净构建（删 `build/release/` 重配），因为 MSBuild / Ninja 有时检测不到 manifest 内容变化

---

## 8. 常见开发场景

**加一个业务字段（例如给 TODO 加 `assignee`）**：

1. `server/internal/model/models.go` 加字段 + JSON tag（`AutoMigrate` 会自动建列）
2. `server/internal/repository/` **通常无需改动**——`TodoRepo.Update(ctx, id, fields map[string]interface{})` 是动态 `Updates(map)`，新增字段只要 handler 把它放进 map 就行；仅当需要新增按该字段查询/排序的专用方法时才改 repo
3. `server/internal/service/` 如果要做字段级校验就加校验；**审计 diff 无需特殊处理**——`audit_service.go` 把整块变更 struct JSON 化写入 `Detail`，新字段自动被记录
4. `server/internal/handler/` DTO 映射（请求体绑定 + 响应序列化），并把新字段加入 Update handler 构造的 map
5. `client/web/src/types/api.ts` 加字段
6. `client/mac/stickytodo/Models/Todo.swift` 加字段（`Codable`，和 JSON tag 同名即可）
7. `client/win/src/models/Todo.h` 加字段 + `client/win/src/codec/JsonHelper.cpp` 的 `ParseTodo` / `TodoToJson` 两处都要补（**不做**就是数据丢失，不是崩溃——见 §7.1）
8. 跑 `smoke.sh` 确认不破坏现有流程；Windows 端跑 `( cd client/win && cmake --preset debug && cmake --build --preset debug && cd build/debug && ctest --output-on-failure -C Debug )` 确认 `test_models_json` + `test_sticky_codec` 的 JSON 往返测试仍过

**加一个 API 端点**：

1. `server/internal/service/` 先写纯业务逻辑
2. `server/internal/handler/` 加 Gin handler
3. `server/internal/router/router.go` 注册路由——鉴权接口挂到 `authed := r.Group("/api")` 下；无需鉴权（如 `/api/login`）直接挂到 `r.` 上
4. `server/scripts/smoke.sh` 里补一步回归
5. 客户端各补一个调用方法：
   - Web：`client/web/src/api/client.ts` 加一个 `api.xxx` 方法 + 必要时 `src/api/queryKeys.ts` 加 cache key + `src/types/api.ts` 加 DTO
   - macOS：`client/mac/stickytodo/Networking/Endpoints.swift` 加 URL 构造器 + `APIClient.swift` 加 `async throws` 方法
   - Windows：`client/win/src/core/HttpClient.h` 加 public 方法声明 + `HttpClient.cpp` 实现（参考现有 `CreateTodo` / `UpdateTodo` / `UpsertSticky` 写法：`WinHttpOpenRequest` → `WinHttpAddRequestHeaders` 加 `Authorization: Bearer` → `WinHttpSendRequest` → `WinHttpReadData` → `JsonHelper` 解析）。**不需要**手写 cache——Windows 端没有 TanStack Query，UI 组件调 `AppState` 的对应方法，`AppState` 调 `HttpClient` 得到结果后通过 `on*Changed` 回调广播给 UI

**加一个 WS 事件类型**（例如"todo.archived"）：

1. `server/internal/ws/event.go` 新增常量 `EventTodoArchived = "todo.archived"`（名称用 `<resource>.<动词过去式>` 格式保持与现有事件一致）
2. `server/internal/service/broadcaster.go` 的 `EventBroadcaster` interface 新增一行签名 `BroadcastTodoArchived(todo any)`（或者根据 payload 形态选 `(id uint)`）——**interface 是权威入口**；`nopBroadcaster` 也要补一行空实现，否则 `var _ EventBroadcaster = nopBroadcaster{}` 编译报错
3. `server/internal/ws/adapter.go` 的 `*HubBroadcaster` 上补实现（`var _ service.EventBroadcaster = (*HubBroadcaster)(nil)` 这行会在漏实现时编译报错兜底），方法体调用 `NewResourceEvent(EventTodoArchived, todo, b.logger)` 或 `NewDeleteEvent(...)`
4. `server/internal/service/todo_service.go`（或 sticky_service.go）在对应写操作的 repo 返回成功之后直接 `s.broadcaster.BroadcastXxx(after)`——**现有 service 层没有显式事务包裹**（`todo_service.go:116,225,247,268,285,303` 全部是"repo 返回即广播"的调用形态），GORM 默认每次 `Updates()` 就是一次隐式事务，本步不引入事务也不要为新事件单独开事务，保持既有调用风格
5. `server/scripts/smoke.sh` 在 Step 33-36 附近加一步 `ws-probe` 校验：触发新操作 → 确认 ws-probe 收到对应 type 的事件
6. 客户端订阅：
   - Web：`src/hooks/useRealtimeSync.ts` 的事件 `switch` 里新增 case，决定 invalidate 哪些 queryKey
   - macOS：`AppState.handleRealtimeEvent` 的 `switch` 里新增 case（sticky.* 直接 merge；todo.* 一般只需 `postTodoNotification` 扇出给 ViewModel 去抖 refresh，不需要在 ViewModel 里为每种事件单独分支）
   - Windows：`client/win/src/core/AppState.cpp` 的 `HandleWsEventOnUIThread` 里新增 `if (event.type == "xxx.yyy")` 分支（sticky.* 直接 merge 到 `stickies_` + 触发 `onStickiesChanged_` 回调；todo.* 通过 `PostMessageToAllStickies(WM_STICKYTODO_REFRESH)` 让所有便签窗口重拉）
7. **不要**给 `todo.updated` 这种"已存在的宽泛事件"再拆分细粒度子事件（如 `todo.title_changed`）——现有架构假设客户端收到 `todo.updated` 就无脑全量 refetch，增加细粒度事件只会让广播膨胀，不会减少客户端请求数

**改 Web UI**：

- 组件在 `client/web/src/components/`，业务数据用 TanStack Query 拿
- 纯前端状态（便签位置、折叠状态、深色模式）放 Zustand
- 跑 `npm run dev`，Vite 会代理 `/api` 到后端，本地前后端分离联调

**改 Windows UI**：

- 控件在 `client/win/src/ui/`，业务数据全都从 `AppState` 拉（`state->GetTodos()` / `state->GetStickies()`），**不要**在 UI 层搞本地缓存
- 所有绘制都走 `D2DRenderer`（Direct2D + DirectWrite），不要引入 GDI / GDI+ 混合绘制——DPI 感知和子像素字体渲染行为会不一致
- 自定义控件（`Button` / `CheckBox` / `TextBox` / `ScrollView` 定义在 `Controls.h/cpp`）用"state 机器 + `Draw(D2DRenderer&)`" 模式。加新状态（如 `disabled` / `selected`）时要同时更新 `HitTest` / `OnMouseMove` / `Draw` 三处
- 调试绘制边界时可以临时在 `Draw` 里 `renderer.DrawRectangle(rect, ColorRGB(1,0,0,0.5), 1.0f)` 画红框，**不要**留到 commit 里
- 本地迭代用 `cmake --preset debug && cmake --build --preset debug`，然后直接 `build/debug/stickytodo.exe` 跑；改完 DTO 或 codec 后跑一次 `( cd build/debug && ctest --output-on-failure -C Debug )` 跑全部 2 个测试（`sticky_codec` + `models_json`）确保往返不破坏

**发版**：见 [docs/RELEASE.md](./docs/RELEASE.md)。简化流程：`git tag v1.2.3 && git push --tags`，CI 会自动把 7 份 server 二进制、macOS DMG、Windows portable zip + setup.exe、Docker 镜像都打好并挂到 Release。
