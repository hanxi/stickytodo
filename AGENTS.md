# AGENTS.md — stickytodo 项目架构

面向开发者 / AI 代理的**工程指南**。安装和使用请看 [README.md](./README.md)。

本文目标：让任何一个新上手的人（或 agent）在读完本文后，就能独立完成改代码、跑测试、发版本这三件事，而不需要再去翻代码猜架构。

详细子文档：

- [docs/server.md](docs/server.md) — 后端架构（分层、API 约定、WS 契约、数据库、embed）
- [docs/client-web.md](docs/client-web.md) — Web 客户端
- [docs/client-mac.md](docs/client-mac.md) — macOS 客户端
- [docs/client-win.md](docs/client-win.md) — Windows 客户端（含 DPI 契约、vcpkg/WinHTTP/Inno Setup 纪律）
- [docs/build-release.md](docs/build-release.md) — 打包脚本、CI workflow、产物矩阵、交叉编译纪律、版本号、macOS Xcode/SDK 锁定
- [docs/dev-notes.md](docs/dev-notes.md) — 验证/回归命令、四端字段同步、常见开发场景
- [docs/RELEASE.md](docs/RELEASE.md) — 发版操作手册

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

- **单二进制分发**：Web UI 通过 `go:embed` 打进后端，部署时不需要 nginx 做静态托管
- **零 CGO**：SQLite 使用 `github.com/glebarez/sqlite`（基于 `modernc.org/sqlite`，纯 Go），可以 `CGO_ENABLED=0` 交叉编译所有平台，配合 alpine 基础镜像产出小体积容器（运行阶段只装了 `ca-certificates` + `tzdata`，无其他系统包）
- **JWT 密钥自管理**：server 首次启动生成 32 字节随机密钥，持久化到 SQLite 的 `app_secrets` 表，**重启不变**，所以用户不需要配环境变量，也不会因重启踢下线

---

## 2. 仓库目录

```
.
├── README.md                  # 用户文档：只讲安装和使用
├── AGENTS.md                  # 本文件：开发者 / 代理架构指南（高层概览 + 索引）
├── CLAUDE.md                  # Claude Code 入口（仅 @AGENTS.md 引用）
├── docs/                      # 详细子文档（见顶部链接列表）
├── assets/branding/           # 品牌视觉资产（单一真相源）
│   ├── stickytodo-icon.svg    # 1024×1024 主 SVG 设计稿（**彩色** brand mark）；AppIcon/favicon 派生
│   ├── stickytodo-menubar.svg # 18×18pt **模板图**（纯黑 + alpha），menubar MenuBarIcon 派生
│   └── out/                   # 脚本生成目录（.gitignore 之外的 AppIcon.icns 等）
├── server/                    # Go 后端（单模块 go.mod）—— 详见 docs/server.md
│   ├── cmd/todo-server/       # main 入口
│   ├── internal/{config,model,repository,service,handler,middleware,router,webui,ws}/
│   ├── scripts/{smoke.sh,ws-probe/}
│   ├── Dockerfile
│   ├── docker-compose.yml
│   └── .env.example
├── client/
│   ├── mac/                   # macOS SwiftUI 客户端（stickytodo.xcodeproj）—— 详见 docs/client-mac.md
│   │   └── stickytodo/{StickyTodoApp.swift,AppState.swift,Models/,Networking/,Storage/,Windows/,Views/}
│   ├── web/                   # React + Vite + Tailwind + Zustand + TanStack Query —— 详见 docs/client-web.md
│   │   ├── src/{api,hooks,store,types,lib,components,views}/
│   │   └── vite.config.ts
│   ├── win/                   # Windows 原生客户端（Win32 + C++/WinRT + Direct2D）—— 详见 docs/client-win.md
│   │   ├── CMakeLists.txt
│   │   ├── CMakePresets.json
│   │   ├── vcpkg.json
│   │   ├── src/{main.cpp,App.*,core/,models/,codec/,ui/,res/}
│   │   └── tests/
│   └── scripts/build.sh       # macOS 客户端本地回归
├── installer/
│   └── setup.iss              # Inno Setup 6 脚本（Windows 安装包；AppId 固定 GUID，勿改）
├── scripts/                   # 打包脚本（本地可单独跑，CI 也复用）—— 详见 docs/build-release.md
│   ├── package-web.sh
│   ├── package-server.sh
│   ├── package-mac-client.sh
│   ├── package-win-client.sh
│   ├── package-docker.sh
│   └── generate-icons.sh
└── .github/workflows/         # 详见 docs/build-release.md
    ├── _build-all.yml         # 可复用 workflow（7 job：web / server-matrix / mac-dmg / win-client-matrix / detect-docker-creds / docker-buildx / publish-release）
    ├── release-tag.yml        # push tag v* → 正式发布
    └── release-branch.yml     # workflow_dispatch → 分支预发布
```

Windows / macOS / Web 三端在**功能**上是对等的，只是实现技术栈不同。

---

## 3. 跨端核心契约

下面三条是**跨整个仓库的硬契约**，改任意一条都需要四端同步：

### 3.1 数据模型

后端 `server/internal/model/models.go` 是唯一真相。字段改动必须同步到 `client/web/src/types/api.ts` + `client/mac/stickytodo/Models/*.swift` + `client/win/src/models/*.h`（含 `codec/JsonHelper.cpp` 的 `Parse*` / `*ToJson` 两个方向）。详见 [dev-notes.md 四端字段必须同步](docs/dev-notes.md#四端字段必须同步)。

四张 GORM 表：`Todo` / `AuditLog` / `AppSecret` / `StickyNote`。详见 [server.md 数据库与迁移](docs/server.md#数据库与迁移)。

### 3.2 API + WebSocket 契约

REST 三类路由（公开 / 鉴权 `/api/*` / WS `/api/ws`），WS 用首帧 auth + close code `4401`/`4400`，5 种事件类型（`todo.created` / `todo.updated` / `todo.deleted` / `sticky.upserted` / `sticky.deleted`）。详见 [server.md API 约定](docs/server.md#api-约定)。

服务端写操作成功后通过 `EventBroadcaster` interface 广播；客户端必须在 WS reconnected 时全量 refetch（hub 不缓冲历史事件）。**广播对称**：发起写请求的客户端本身也会收到同一事件 —— 各端 mutation 必须用"服务端响应直接写 cache"实现本端即时反馈，不能依赖 WS 绕一圈回来。

### 3.3 便签的本机 vs 云端边界

`/api/sticky-notes` 是**唯一**的便签数据源（云端权威），但 `frame` 字段（窗口位置）属于**本机 UI 偏好不跨端同步**：

- macOS：`FrameStore` 写 `UserDefaults`，key `stickytodo.frames`
- Windows：`FrameStore` 写 `%LocalAppData%\stickytodo\frames.json`
- Web：不维护窗口位置（浏览器里"便签"本质是一张 Card）

两端 `PUT /api/sticky-notes/:id` 请求体里的 `frame` 字段**恒为 `"{}"`**——保留这个字段是为了服务端 schema 稳定，不要因为"客户端不用"就删除它或改为可空。

---

## 4. 验证、构建、发布

### 验证（每次改完都跑）

```bash
# 后端 36 步冒烟（含 WS 回归）
./server/scripts/smoke.sh

# macOS 客户端 Xcode build
./client/scripts/build.sh
```

完整命令清单（含 Web typecheck、Windows ctest、Go vet）见 [dev-notes.md 验证和回归](docs/dev-notes.md#验证和回归)。

### 构建与发布

- 本地打包：5 个 `scripts/package-*.sh`（web / server / mac-client / win-client / docker）
- CI：`.github/workflows/_build-all.yml` 7 个 job，输出 server 7 份二进制 + Mac DMG + Windows zip×2 + setup.exe×2 + Docker 多架构镜像
- 发版：`git tag v1.2.3 && git push --tags`

详细产物矩阵、CI 拓扑、交叉编译纪律、版本号来源、macOS Xcode/SDK 锁定策略，全部在 [build-release.md](docs/build-release.md)。

发版手册见 [docs/RELEASE.md](docs/RELEASE.md)。
