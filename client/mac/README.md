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
- **全局历史**：进入 **⌘, → 「历史」Tab** 查看所有操作审计（独立入口已从菜单栏面板移入设置窗口，见 `MenuBarContent.swift` 头部注释）；单条 TODO 行尾部的 `⋯`（`ellipsis.circle` 图标）按钮展开菜单后选「历史」即可查看该条的变更轨迹（sheet 形式，作用域仅该条 todo）。
- **WebSocket 实时同步**：登录后 `RealtimeClient` 与服务端 `/api/ws` 建立长连接，接收 `todo.*` / `sticky.*` 事件并通过 `NotificationCenter` / `@Published` 驱动各 `StickyViewModel` 去抖 refetch；与 Web 端共享完全一致的首帧 auth 协议、`[1,2,4,8,16,30]s` 指数退避、close code `4401` → `.unauthorized` 语义。

## 目录结构

```
client/mac/
├── stickytodo.xcodeproj/        # Xcode 工程
└── stickytodo/                  # 源码
    ├── StickyTodoApp.swift      # @main 入口 + MenuBarExtra + Settings
    │                            # 同文件内定义 StickyWindowBridge：Combine sink 订阅
    │                            # AppState.$stickies / $isAuthenticated，驱动 WindowManager
    ├── AppState.swift           # @MainActor 全局状态：认证 + 云端 stickies 快照 +
    │                            # APIClient + RealtimeClient + FrameStore；
    │                            # 负责 WS 事件路由（sticky.* merge 进 @Published，
    │                            # todo.* 通过 NotificationCenter 扇出给 ViewModel）
    ├── Info.plist               # LSUIElement=YES 等
    ├── stickytodo.entitlements  # App Sandbox / network.client / files.user-selected.read-only
    ├── Models/                  # Todo / AuditLog / Filter / StickyNote
    │                            # StickyNote.id=String、无 frame、带 createdAt/updatedAt
    ├── Networking/              # Endpoints（URL 构造）
    │                            # APIClient（URLSession + async/await，含 listStickies /
    │                            #   upsertSticky / deleteSticky / getSticky，内部完成
    │                            #   CodableRGBA↔JSON、TodoFilter↔snake_case JSON 编解码）
    │                            # RealtimeClient（URLSessionWebSocketTask：首帧 auth /
    │                            #   ready / 指数退避 / 15s 本端 ping 兜底）
    ├── Storage/                 # KeychainStore（JWT，kSecAttrAccessibleAfterFirstUnlock）
    │                            # FrameStore（便签窗口位置，[String: CodableRect] →
    │                            #   UserDefaults key "stickytodo.frames"；纯本机，不上云）
    ├── Windows/                 # StickyWindowController（单个 NSWindow，.floating 置顶、
    │                            #   无标题栏、frame 由 Manager 从 FrameStore 注入）
    │                            # StickyWindowManager（便签 id → NSWindow 的集合管理）
    └── Views/                   # StickyView / StickyViewModel（订阅 4 个 Notification
                                 #   去抖 refresh） / TodoRow / DraftTodoRow / FilterEditor /
                                 # HistoryView / MenuBarContent / SettingsView / WindowDragHandle
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

Settings Scene 是一个 **TabView**，包含三个 Tab：「设置」/「历史」/「关于」（见 `SettingsView.swift` 的 `TabView`）。首次配置全在「设置」Tab：

1. 点击菜单栏 `note.text` 图标 → **「打开设置」**（或按 **⌘,**），进入「设置」Tab
2. 「服务器」区块填 `http://127.0.0.1:8080`，点 **「保存地址」** 按钮
   - 只填 `127.0.0.1:8080` 也可以，保存时会自动补 `http://`
3. 可选：点同区块的 **「测试连接」** 按钮，客户端会向该地址发 `GET /health`，成功会在按钮下方以绿色文字展示 server 名称与版本，失败则展示红色错误文案
4. 「账号」区块填 username / password（与后端 `.env` 的 `TODO_USERNAME` / `TODO_PASSWORD` 一致），点 **「登录」** 按钮（或在密码框按 **Return**）
5. 回到菜单栏面板，点 **「新建便签」** 或按 **⌘N**

> 「历史」Tab 已登录后内嵌全局 `HistoryView`；「关于」Tab 展示品牌信息、版本号（来自 `Info.plist` 的 `CFBundleShortVersionString` / `CFBundleVersion`）、Bundle ID 与项目链接。

## 快捷键

| 快捷键 | 作用 | 生效条件 / 绑定位置 |
|---|---|---|
| **⌘,** | 打开设置窗口 | SwiftUI `Settings` Scene 自动注册：macOS 14+ 走 `SettingsLink`；macOS 13 回退到 `NSApp.sendAction(#selector(showSettingsWindow:))`（见 `MenuBarContent.swift#settingsButton`）。App 处于激活态时按键即可，菜单栏面板无需展开 |
| **⌘N** | 新建便签 | 菜单栏面板展开后由「新建便签」按钮绑定（`MenuBarContent.swift:92`：`.keyboardShortcut("n", modifiers: [.command])`） |
| **⌘Q** | 退出 App | 菜单栏面板展开后由「退出」按钮绑定（`MenuBarContent.swift:137`：`.keyboardShortcut("q", modifiers: [.command])`，按钮内部调用 `NSApplication.shared.terminate(nil)`）。**云端数据源重构后，进程退出不再需要"快照落盘"**——便签数据本身已是服务端权威；窗口位置由 `StickyWindowController` 的 `didMove` / `didResize` 回调每次触发时立即 `frameStore.save(id:rect:)` 同步写入 UserDefaults（`save` 方法本身无缓冲，写一次就落一次），所以用户拖动/缩放停下的瞬间状态已经持久化 |
| **Return** | 提交当前 sheet 表单（登录 / 新建 Todo / 编辑 Todo / 保存筛选） | 由 sheet 内主按钮的 `.keyboardShortcut(.defaultAction)` 承担（例：`SettingsView.swift:219` 的「登录」按钮） |
| **Esc** | 取消 / 关闭当前 sheet | sheet 内取消按钮或 HistoryView 的「关闭」按钮绑定 `.keyboardShortcut(.cancelAction)`（例：`HistoryView.swift:65`） |

> 说明：由于本 App 是菜单栏常驻（无 Dock 图标），⌘N / ⌘Q **仅在 MenuBarExtra 面板展开时**才挂载到响应链——面板折叠时按钮视图树不参与渲染，快捷键不会命中，这与系统级全局快捷键（如 ⌘,）不同。如需无面板情况下退出 App，请在 MenuBarExtra 菜单栏图标上点击 → 展开面板 → 按 ⌘Q；或改走 Activity Monitor 强制结束进程。

## API 对齐

- **所有 HTTP 请求**统一在 `Networking/APIClient.swift`，**所有 URL 拼装**统一在 `Networking/Endpoints.swift`，**所有 WebSocket 事件**走 `Networking/RealtimeClient.swift`。
- **DTO（与后端 `server/internal/model/models.go` + `server/internal/handler/*` 一一对应）**：
  - `Models/Todo.swift`：`Todo` / `TodoStatus` / `TodoListResponse` / `CreateTodoRequest` / `UpdateTodoRequest` / `DeleteTodoResponse` / `TagListResponse`
  - `Models/AuditLog.swift`：`AuditLog` / `AuditAction` / `AuditListResponse`
  - `Models/StickyNote.swift`：`StickyNote`（id=String，无 frame 字段，带 createdAt/updatedAt）/ `CodableRect`（FrameStore 用）/ `CodableRGBA`（便签背景色，与后端 `bg_color` 的 `{red,green,blue,alpha}` JSON 字段逐项对齐）
  - `Networking/APIClient.swift`（随 `APIClient` 一起定义，仅在网络层使用的 DTO）：`LoginRequest` / `LoginResponse` / `HealthResponse` / `EmptyResponse` / `DeleteStickyResponse` / `APIError`；私有 `TodoFilterDTO`（snake_case 适配层，桥接 `TodoFilter` 的 camelCase CodingKeys 与后端 JSON）
- **客户端特有的 model**（本地状态，不与后端直接对应）：
  - `Models/Filter.swift`：`TodoFilter`（便签筛选条件 + 构造 `GET /api/todos` 的查询参数；通过 `TodoFilterDTO` 序列化成跨端兼容的 snake_case JSON 塞进 `sticky.filter`）
- **便签数据流**（云端数据源重构后）：`listStickies()` 登录后全量拉取 → `@Published var stickies` → `StickyWindowBridge.sink` → `StickyWindowManager` diff 出窗口增删；本地的 `addSticky / updateSticky / removeSticky` 都是 `async throws`，**先落服务端再改本地**（保守的"先写后读"，**失败时不做乐观更新**）。失败呈现分两条路径：
    - 在便签窗口内部发起的 `updateSticky / removeSticky`（改标题 / 换色 / 改筛选 / 删除）：错误写入 `StickyViewModel.currentError`，由 `StickyView.swift:102` 的 `.alert(item:)` 弹系统 Alert 展示（`StickyViewError` 是 `Identifiable, Equatable`，可驱动 alert presentation）。
    - 在菜单栏面板内发起的 `addSticky`（点「新建便签」/ ⌘N）：当前只在 `MenuBarContent.swift:81` 以 `print("[MenuBarContent] addSticky failed: ...")` 落系统日志；菜单栏面板不弹 alert。若希望感知失败，需自行到 Console.app 过滤进程名为 `stickytodo`。
  
  **`StickyNote` 不再走 UserDefaults**；只有窗口位置（`FrameStore`）仍走 UserDefaults，因为它是纯本机 UI 偏好。
- **WebSocket 事件**（与 `server/internal/ws/event.go` 对齐的 5 种）：`todo.created` / `todo.updated` / `todo.deleted` / `sticky.upserted` / `sticky.deleted`。`RealtimeClient` 将其解码成 `RealtimeEvent` 透传给 `AppState.handleRealtimeEvent`：sticky.* 直接 merge 到 `stickies`，todo.* 通过 `Notification.Name.stickyTodoCreated / Updated / Deleted` 扇出给各个 `StickyViewModel` 做 300ms 去抖 refetch；重连成功会发 `.stickyRealtimeReconnected` 触发所有 ViewModel 全量刷新。
- 接口列表、错误码、请求体形状、WS 协议细节见 [../../server/README.md](../../server/README.md)。

## 验证 / 回归

从仓库根执行以下两条命令，都应以退出码 0 结束。

**前置**：`smoke.sh` 只发 HTTP 请求，**不会自己启动 server**。请在另一个终端先把 server 起起来（`cd server && set -a && . ./.env && set +a && go run ./cmd/todo-server`，或 `cd server && docker compose up -d`）。下面的命令使用 shell 参数默认值语法 `"${TODO_USERNAME:-admin}"`：若当前 shell 已导入过 `.env`，会优先用你的真值；否则回退到 `.env.example` 的占位 `admin`/`test123`。

```bash
# 1) 后端端到端冒烟（36 步，覆盖 /health / login / todo CRUD / complete / reopen /
#    history / tags / 软删 / 恢复 / audit / 401 & 400 & 404 分支 /
#    sticky-notes CRUD / WebSocket 回归（Step 33-36：未发 auth 4401、bad token 4401、
#    合法 auth → ready、REST 触发后收到 todo.created 实时推送）。
#    脚本启动时会自动 `go build ./scripts/ws-probe` 产出一个临时 WS 探针二进制。
#
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
- **无离线写队列**：`RealtimeClient` 本身有指数退避自动重连（`[1,2,4,8,16,30]s`），并在 `reconnected` 信号触发后由 `AppState` 广播 `.stickyRealtimeReconnected` Notification 让所有 `StickyViewModel` 全量 refetch——所以**读数据**在网络恢复后会自愈。但离线期间用户发起的 `addSticky / updateSticky / removeSticky` 等**写操作**会直接抛错（由 `APIError.userMessage` 展示），并不会暂存到本地队列等上线后回放。需要离线写场景时，请考虑在 APIClient 之上再加一层乐观队列。
- 未实现自动更新；新版本需要手动替换 `.app`。

## License

MIT（见仓库根 README）。
