# stickytodo（macOS 客户端）

一个菜单栏常驻的 TODO 便签 App，品牌名 **StickyTodo**，基于 **SwiftUI + AppKit**，通过 HTTP 连接 [server/](../../server) 提供的 Go 后端。属于 `stickytodo` 项目的 macOS 端（`github.com/hanxi/stickytodo`）。

## 主要特性

- **菜单栏常驻**：`LSUIElement=YES`，无 Dock 图标；点击菜单栏 `note.text` 图标展开面板。
- **多便签**：每条便签一个独立 NSWindow，`.floating` 层级置顶，支持拖动、缩放、换色。
- **独立筛选**：每个便签持有自己的 `TodoFilter`（状态 / 标签 / 关键词 / 截止时间 / 已删除可见性 / 分页），互不影响。
- **云端数据源 + 本地缓存**：
  - 便签内容（标题 / 颜色 / 筛选）→ 服务端 `/api/sticky-notes`（唯一数据源），登录后通过 `listStickies` 全量拉取，后续变更通过 WebSocket 事件实时同步。
  - 便签窗口位置 → 本机 UserDefaults（key = `stickytodo.frames`，由 `FrameStore` 管理），属于纯本机 UI 偏好，不跨设备同步。
  - 登录 token → Keychain（`kSecAttrAccessible = kSecAttrAccessibleAfterFirstUnlock`），重启即保持登录。
- **全局历史**：菜单栏面板「历史」按钮查看所有操作审计；单条 TODO 行尾部的 `⋯`（`ellipsis.circle` 图标）按钮展开菜单后选「历史」即可查看该条的变更轨迹。

## 目录结构

```
client/mac/
├── stickytodo.xcodeproj/        # Xcode 工程
└── stickytodo/                  # 源码
    ├── StickyTodoApp.swift      # @main 入口 + MenuBarExtra + Settings + StickyWindowBridge（Combine sink 订阅 AppState）
    ├── AppState.swift           # 全局状态（认证 + 云端便签列表 + APIClient + RealtimeClient + FrameStore）
    ├── Info.plist               # LSUIElement=YES 等
    ├── stickytodo.entitlements  # App Sandbox / network.client / files.user-selected.read-only
    ├── Models/                  # 数据模型（Todo / AuditLog / Filter / StickyNote）
    ├── Networking/              # Endpoints（URL 构造）+ APIClient（async/await）+ RealtimeClient（WebSocket）
    ├── Storage/                 # KeychainStore（JWT）+ FrameStore（本机窗口位置 UserDefaults）
    ├── Windows/                 # StickyWindowController（NSWindow 壳）+ StickyWindowManager（diff 同步）
    └── Views/                   # StickyView / TodoRow / FilterEditor / HistoryView
                                 # MenuBarContent / SettingsView / StickyViewModel
```

## 前置依赖

- macOS 13.0+（`Info.plist` 中 `LSMinimumSystemVersion=13.0`）
- **Xcode.app**（完整 IDE，不是仅 Command Line Tools；`xcodebuild` 需要 macOS SDK）。实测版本：`Xcode 26.4 / Build 17E192`。更低版本未实测，若遇编译错误请优先升级 Xcode。
- 后端已启动并可访问，见 [../../server/README.md](../../server/README.md)

## 构建 & 运行

以下命令的工作目录均从**仓库根**（`todo/`）开始。

### 方式一：Xcode

1. 打开 `client/mac/stickytodo.xcodeproj`
2. 选择 **My Mac** 作为目标，按 **⌘R** 运行
3. App 启动后菜单栏出现 `note.text` 图标

### 方式二：xcodebuild 命令行

```bash
cd client/mac
# Debug 构建：使用 Xcode 的 "Sign to run locally" 临时签名（CODE_SIGN_IDENTITY="-"），
# 仅能在本机运行，发布时需要换成 Developer ID / App Store 正式签名。
xcodebuild \
  -project stickytodo.xcodeproj \
  -scheme stickytodo \
  -configuration Debug \
  -destination 'platform=macOS' \
  -derivedDataPath /tmp/stickytodoBuild \
  CODE_SIGN_IDENTITY="-" CODE_SIGNING_REQUIRED=NO CODE_SIGNING_ALLOWED=NO \
  clean build
# 产物位置：/tmp/stickytodoBuild/Build/Products/Debug/stickytodo.app
```

### 方式三：一键回归脚本（推荐用于 CI / 本地冒烟）

```bash
# 从仓库根执行
./client/scripts/build.sh
# 或指定 Release 配置
CONFIG=Release ./client/scripts/build.sh
```

脚本会：
1. 清理 `/tmp/stickytodoBuild`
2. 运行 `xcodebuild clean build`
3. 校验日志中含 `** BUILD SUCCEEDED **` 且产物 `.app` 已生成
4. 任一步失败即退出非 0，并把 `xcodebuild` 最后 60 行日志打印到 stderr

## 首次配置

1. 点击菜单栏 `note.text` 图标 → **「打开设置」**（或按 **⌘,**）
2. 「服务器」表单填 `http://127.0.0.1:8080`，点「保存地址」
   - 只填 `127.0.0.1:8080` 也可以，会自动补 `http://`
3. 可选：点「服务器」表单下方的 **「测试连接」** 按钮，客户端会向该地址发 `GET /health`，成功会展示 server 名称与版本
4. 「账号」表单填 username / password（与后端 `.env` 的 `TODO_USERNAME` / `TODO_PASSWORD` 一致）→ **登录**
5. 回到菜单栏面板，点 **「新建便签」** 或按 **⌘N**

## 快捷键

| 快捷键 | 作用 | 生效条件 |
|---|---|---|
| **⌘,** | 打开设置窗口 | Settings Scene 本身注册了 `showSettingsWindow:` selector，只要 App 处于激活态均可 |
| **⌘N** | 新建便签 | 菜单栏面板已展开时（由 `MenuBarContent` 里的「新建便签」按钮绑定） |
| **⌘Q** | 退出 App | 菜单栏面板已展开时（其他退出路径，如 `Cmd+Q` 在设置窗口、或 `Dock` 退出，也都会触发 `NSApplication.willTerminateNotification`，AppState 会在终止前把便签快照落盘） |
| **Return** | 提交当前 sheet 表单（登录 / 新建 Todo / 编辑 Todo / 保存筛选） | sheet 打开时 |
| **Esc** | 取消 / 关闭当前 sheet | 所有 sheet 的「取消」按钮（或 HistoryView 里的「关闭」按钮）均绑定 `.keyboardShortcut(.cancelAction)` |

> 说明：由于本 App 是菜单栏常驻（无 Dock 图标），⌘N / ⌘Q 的事件路由依赖 MenuBarExtra 面板已被用户打开时建立的按钮响应链，这与系统级全局快捷键不同。

## API 对齐

- **所有 HTTP 请求**统一在 `Networking/APIClient.swift`，**所有 URL 拼装**统一在 `Networking/Endpoints.swift`。
- **DTO（与后端 `server/internal/model/models.go` + `server/internal/handler/*` 一一对应）**：
  - `Models/Todo.swift`：`Todo` / `TodoStatus` / `TodoListResponse` / `CreateTodoRequest` / `UpdateTodoRequest` / `DeleteTodoResponse` / `TagListResponse`
  - `Models/AuditLog.swift`：`AuditLog` / `AuditAction` / `AuditListResponse`
  - `Networking/APIClient.swift`（随 `APIClient` 一起定义，仅在网络层使用的 DTO）：`LoginRequest` / `LoginResponse` / `HealthResponse` / `EmptyResponse` / `APIError`
- **客户端特有的 model**（本地状态，不与后端直接对应）：
  - `Models/Filter.swift`：`TodoFilter`（便签筛选条件 + 构造 `GET /api/todos` 的查询参数）
  - `Models/StickyNote.swift`：`StickyNote` / `CodableRect` / `CodableRGBA`（便签窗口本地持久化结构，仅 UserDefaults 存，从不上传服务端）
- 接口列表、错误码、请求体形状见 [../../server/README.md](../../server/README.md)。

## 验证 / 回归

从仓库根执行以下两条命令，都应以退出码 0 结束。

**前置**：`smoke.sh` 只发 HTTP 请求，**不会自己启动 server**。请在另一个终端先把 server 起起来（`cd server && set -a && . ./.env && set +a && go run ./cmd/todo-server`，或 `cd server && docker compose up -d`）。下面的命令使用 shell 参数默认值语法 `"${TODO_USERNAME:-admin}"`：若当前 shell 已导入过 `.env`，会优先用你的真值；否则回退到 `.env.example` 的占位 `admin`/`test123`。

```bash
# 1) 后端端到端冒烟（21 步，覆盖 /health / login / CRUD / complete / reopen /
#    history / tags / 软删 / 恢复 / audit / 401 & 400 & 404 分支）。
#    下面的 admin / test123 只是 .env.example 的占位值，若已 `set -a; . ./.env; set +a`
#    导入过环境变量，shell 的参数默认值语法会优先用你的真值。
BASE_URL=http://127.0.0.1:8080 \
  TODO_USERNAME="${TODO_USERNAME:-admin}" \
  TODO_PASSWORD="${TODO_PASSWORD:-test123}" \
  ./server/scripts/smoke.sh

# 2) 客户端 Xcode clean + build（Debug；临时签名 CODE_SIGN_IDENTITY="-"）
./client/scripts/build.sh
```

两个脚本任一失败都会把错误原因打印到 stderr。本仓库在每个阶段收尾时都会跑一遍这两个脚本，作为"阶段完成"的硬门槛。

## 已知局限 / Roadmap

- 当前后端是单账号（环境变量配置），客户端的 Keychain 也按账号维度隔离，但 UI 只呈现单账号工作流；多账号需要后端先扩展。
- 便签窗口 `.floating` 层级高于应用设置窗口：编辑 sheet 在便签内部弹出无问题；若未来要做 popover 选择器等超出便签 frame 的面板，需要临时降 level。
- 未实现自动重连 / 离线队列；网络断开时操作会直接报错（由 `APIError.userMessage` 展示）。
- 未实现自动更新；新版本需要手动替换 `.app`。

## License

MIT（见仓库根 README）。
