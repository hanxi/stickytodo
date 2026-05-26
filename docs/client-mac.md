# macOS 客户端（client/mac/）

技术栈：**Swift 5.9 + SwiftUI + Combine + Keychain Services**。`@Published` 在多数视图里以 SwiftUI 的 `@EnvironmentObject` / `@ObservedObject` 方式消费；只有 `StickyWindowBridge` 显式 `import Combine` 用 `sink` + `AnyCancellable` 直接订阅——因为它不是 View，挂在 SwiftUI 生命周期里会在 MenuBarExtra 面板折叠时失去响应。

---

## Bundle 和命名

真值来自 `stickytodo.xcodeproj/project.pbxproj`：

- **Bundle ID**：`com.hanxi.stickytodo`（`PRODUCT_BUNDLE_IDENTIFIER`）
- **部署目标**：macOS 13.0+（`MACOSX_DEPLOYMENT_TARGET = 13.0`）
- **版本号**：`MARKETING_VERSION = 1.0`（当前硬编码在 pbxproj，`package-mac-client.sh` **不修改** Info.plist，只把 `$VERSION` 打进产物文件名）
- **Keychain service name**：`com.hanxi.stickytodo`（`KeychainStore.service`，存储 JWT）
- **UserDefaults**：`UserDefaults.standard`（**非** App Group suite），本机窗口位置持久化 key 是 `stickytodo.frames`（`FrameStore.defaultsKey`）——便签业务数据本身已改走服务端，不再落 UserDefaults
- **`os.Logger` subsystem**：`com.hanxi.stickytodo`
- **菜单栏图标**：Assets.xcassets 中的 **`MenuBarIcon`**（template image，由 `scripts/generate-icons.sh` 从 `assets/branding/stickytodo-menubar.svg` 渲染产出）；**无 Dock 图标**（Info.plist `LSUIElement=YES`）
- **模板图（template image）规则**：`MenuBarIcon.imageset/Contents.json` 里必须有 `"properties":{"template-rendering-intent":"template"}`（注意 `properties` 是顶层字段，不是塞在每张图里）。这样系统才会自动按"明/暗菜单栏 + 选中态"反色。源 SVG 只能用纯黑 `#000000` + alpha；**不要**在 SVG 里画彩色——主 brand mark 的黄/绿配色属于 AppIcon，不是 menubar
- **App 图标**（Dock / Finder / About / DMG）：Assets.xcassets 中的 **`AppIcon`**（`ASSETCATALOG_COMPILER_APPICON_NAME = AppIcon`）；SwiftUI 侧在 `StickyTodoApp.swift` 用 `Image("MenuBarIcon")`（注意是名称加载，不是 `systemName:`；写成 `Image(systemName: "MenuBarIcon")` 会被当作 SF Symbol 查不到而显示占位）

---

## 模块职责

### AppState

`@MainActor` + `ObservableObject`，持有 `APIClient`、登录态、云端便签列表、`FrameStore`、`RealtimeClient`。关键职责分两层：

- **认证 / 数据**：`login(...)` 成功后立即 `bootstrapAfterAuth()` → 并行全量拉 `listStickies()` + 启动 `RealtimeClient.connect()`；`logout()` 同步 `RealtimeClient.disconnect()` + 清 stickies
- **WS 事件路由**：`sticky.upserted` / `sticky.deleted` 直接 merge 到 `@Published var stickies: [StickyNote]`；`todo.*` 通过 `NotificationCenter.default.post(name: .stickyTodoCreated/Updated/Deleted, userInfo: [AppStateNotification.todoKey: event])` 广播给各个 `StickyViewModel`——ViewModel 不直接耦合 AppState，解耦点就在这组 `Notification.Name` 常量上

### Networking

- **`APIClient`**：纯 `URLSession`（async/await），方法签名与后端 REST 一一对应，失败时抛结构化 `APIError`。sticky 方法（`listStickies` / `getSticky` / `upsertSticky` / `deleteSticky`）内部通过 `encodeBgColor` / `encodeFilter` 完成"CodableRGBA ↔ JSON 字符串"和"TodoFilter ↔ snake_case JSON 字符串"双向转换；TodoFilterDTO 是私有的 snake_case 适配层（Web 端 `filterToJSON` 直接 `JSON.stringify(filter)` 产出 snake_case，macOS `TodoFilter.CodingKeys` 是 camelCase，两端互通必须走这一层）
- **`RealtimeClient`**：`URLSessionWebSocketTask` 实现的 WS 客户端，与 Web 端 `stickyWS` 协议行为等价（首帧 auth / 2s 超时 / ready 帧 / 指数退避 `[1,2,4,8,16,30]s` / close code 4401 → `.unauthorized` signal）。额外的"客户端侧主动 ping"（15s）用于在后端服务器 30s ping 之外做保活兜底——`URLSessionWebSocketTask` 不会自动响应服务端 ping，必须有本端 ping 才能维持连接活性。事件用 `RealtimeEvent` struct 透传（`data: Data?` 是原始 JSON bytes，由订阅者按需解码；`id: String?` 统一把 uint 和 string 两种主键转为字符串表示）

### Storage

- **`KeychainStore`**：JWT 读写。Accessible 级别 `kSecAttrAccessibleAfterFirstUnlock`——首次解锁后就能访问，适合菜单栏常驻型 App
- **`FrameStore`**：便签窗口位置的**纯本机**持久化。key `stickytodo.frames`，value 是 `[String: CodableRect]` 的 JSON。`StickyNote` 已经不再携带 frame 字段（属于"本机 UI 偏好，不跨设备同步"）；`StickyWindowController` 的 `didMove/didResize` 回调写入这里，`StickyWindowManager.sync` 开新窗口时从这里查（未命中则用 `defaultFrame + 偏移`兜底）

### Windows/

**仅两个文件**（`StickyWindowController.swift` + `StickyWindowManager.swift`）：

- **`StickyWindowController`**：负责**单个**便签窗口（`window.level = .floating` 实现桌面置顶、`init(note:initialFrame:contentBuilder:)` 签名——frame 由 Manager 从 FrameStore 查出后注入，不从 note 读；把 SwiftUI `StickyView` 注入 `NSHostingView`）
- **`StickyWindowManager`**：负责**多个**窗口集合，`init(frameStore:contentBuilder:)` 注入 FrameStore，按 sticky id 建立窗口，新增/关闭便签时增删对应 `NSWindow`

⚠️ **`StickyWindowBridge` 不在 Windows/ 目录下**，而是定义在 `StickyTodoApp.swift` 同文件内（`final class StickyWindowBridge: ObservableObject`），作为 App 与 WindowManager 的响应式桥梁；Bridge 的三个回调（`onNewSticky` / `onCloseSticky` / `onNoteChange`）都把 `appState.addSticky/removeSticky/updateSticky` 的 async API 包成 `Task { @MainActor do-catch }` 调用。

**订阅机制**：`attach(appState:)` 通过 `import Combine` 的 `appState.$stickies.sink(...)` / `appState.$isAuthenticated.sink(...)` 订阅两个 `@Published` 源，cancellable 持有在 Bridge 自身——**不能**改用 SwiftUI 的 `.onChange` 挂在 `MenuBarExtra { } ` 内部，因为 MenuBarExtra 面板未展开时子树未挂载，`.onChange` 不求值，会导致便签被另一端通过 WS 删除/新增后本机窗口不同步，直到用户点开菜单栏才 catch up（历史 bug）。

### Views/（共 9 个文件）

- **`MenuBarContent.swift`**：菜单栏点出的主面板。**当前布局三段**：
  - ①`headerRow`——品牌标题 + 已登录时在尾部展示用户名
  - ②中段 `authenticatedBody` / `unauthenticatedBody`——已登录时只有**一个**「新建便签」按钮（独占一行、全宽、**`.bordered` 样式**，绑定 `⌘N`。从 `.borderedProminent` 改 `.bordered` 的原因代码注释已写：prominent 按下会切成高亮填充 + 白色前景，深浅色交叉下观感失衡）；未登录时展示一段"尚未登录。请在『设置』中配置服务器地址并登录。"提示 + 一个 **`.borderedProminent`** 样式的「打开设置」按钮
  - ③`footerRow`——无论是否登录都挂在底部：`[设置] [登出（仅已登录）] [退出 ⌘Q]`，其中「退出」是 `.destructive`
  
  **历史入口已整体迁移到 `SettingsView` 的「历史」Tab，MenuBarContent 里不再有「历史」按钮**（文件头注释明确写着"历史查看器已迁移到 Settings → 历史 Tab"）。新建便签的失败路径仅 `print("[MenuBarContent] addSticky failed: ...")`，不弹 alert
- **`StickyView.swift`**：单个便签的 SwiftUI 根视图；用 `@StateObject private var viewModel: StickyViewModel` 持有业务逻辑。`onCloseSticky` / `onNoteChange` 回调签名里的 sticky id 类型是 `String`（不是 UUID）。错误呈现靠 `.alert(item: $viewModel.currentError)`，由 ViewModel 的 `@Published var currentError: StickyViewError?` 驱动
- **`StickyViewModel.swift`**：`final class StickyViewModel: ObservableObject`，承载单个便签的 TODO 列表、加载状态、错误态（`StickyViewError: Identifiable, Equatable`）等 `@Published` 字段；由 `StickyView` own 其生命周期。**在 `init` 里订阅 4 个 NotificationCenter 事件**（`.stickyTodoCreated/Updated/Deleted/.stickyRealtimeReconnected`），任一事件到来都触发 `scheduleDebouncedRefresh`（300ms 窗口合并多事件为一次 `refresh()`）。observer tokens 用 `nonisolated(unsafe)` 存储以便 `deinit` 能 remove
- **`SettingsView.swift`**：`⌘,` 打开的 Settings Scene，**标准 macOS Preferences 风格的 `TabView`**，固定尺寸 `520×420`（在 `body` 上 `.frame(width: 520, height: 420)`），共 **3 个 Tab**：
  - 「设置」（`generalTab`）：服务器 Base URL 表单（`urlDraft` + 合法性状态 `URLValidationState`，「保存地址」会把 `http://` 自动前缀补全 + 「测试连接」→ `GET /health` + 绿色/红色结果文案）+ 账号表单（未登录→用户名/密码登录；已登录→展示账号 + 登出）
  - 「历史」（`historyTab`）：已登录时嵌入 `HistoryView(mode: .global, apiClient: appState.apiClient, embedded: true)`（`embedded: true` 会让 HistoryView 不渲染自己的「关闭」按钮，由外层 Settings 窗口统一关闭）；**未登录时**展示锁图标 + 文案「请先在『设置』Tab 登录后查看历史」的占位视图
  - 「关于」（`aboutTab`）：`Form + formStyle(.grouped)` 风格，内嵌 `aboutBlock`，展示品牌信息、版本号（来自 Info.plist 的 `CFBundleShortVersionString` / `CFBundleVersion`）、Bundle ID、项目链接
- **`TodoRow.swift`** / **`DraftTodoRow.swift`**：已存 TODO / 新建草稿 TODO 的行组件
- **`FilterEditor.swift`**：便签绑定的筛选条件编辑器
- **`HistoryView.swift`**：变更历史 / 审计日志视图。**两种展示模式**由 `Mode` enum 区分（`.todo(id:title:)` / `.global`）；另有一个 `embedded: Bool = false` 开关——`false`（默认）以独立 `.sheet` 形式弹出、顶部渲染「关闭」按钮（依赖 `@Environment(\.dismiss)`）；`true`（嵌入 Settings TabView）时顶部不渲染关闭按钮，由外层 Settings 窗口统一关闭
- **`WindowDragHandle.swift`**：便签顶部不可见的拖动区（便签窗口 `styleMask = [.borderless, .resizable, .fullSizeContentView]`，**无系统标题栏**，靠这里拖动）

### Models/（共 4 个）

与后端 `models.go` + `types/api.ts` 一一对应：`Todo.swift` / `AuditLog.swift` / `StickyNote.swift` / `Filter.swift`。`StickyNote.id: String`（客户端生成 UUID 字符串，由 `StickyNote.newID()` 产出）；**不包含 frame 字段**。

### StickyTodoApp.swift

`@main` 入口，持有**两个** `@StateObject`：

- **`appState: AppState`**：纯数据/业务状态
- **`windowBridge: StickyWindowBridge`**：App 初始化时立刻把 `AppState.stickies` / `isAuthenticated` 的变化同步到 `StickyWindowManager`——之所以必须在 `App.init()` 就建好，是因为 `MenuBarExtra` 的 `.onAppear` 只有用户点开菜单栏面板才触发，太晚

body 只有两条 Scene：

```swift
MenuBarExtra { MenuBarContent() } label: { Image(systemName: "note.text") }
    .menuBarExtraStyle(.window)
Settings { SettingsView() }
```

`Image(systemName:)` 走 SF Symbols，缺了 `systemName:` 会去 Asset Catalog 找同名图片。`.menuBarExtraStyle(.window)` 决定了点菜单栏图标弹出的是一个**浮窗**而非系统菜单。

⚠️ **这里绝对不能把"`appState.stickies` / `isAuthenticated` 变化 → 调 `windowBridge.syncWindows`"写成 SwiftUI 的 `.onChange` 挂在 `MenuBarExtra { }` 内部**——菜单栏面板未展开时整个子树不挂载、`.onChange` 不求值，会导致 WS 推送的 sticky 增删不能实时驱动桌面便签窗口更新。Bridge 用 Combine sink 自主订阅即可，StickyTodoApp.body 里**不需要**任何 onChange。

---

## 快捷键

- **`⌘,`** 打开设置：SwiftUI `Settings` Scene 自带的系统级快捷键，App 激活时即可命中，**不依赖菜单栏面板是否展开**；代码里没有也不需要手动 `.keyboardShortcut(",")`。macOS 14+ 走 `SettingsLink`，13 回退到 `NSApp.sendAction(#selector(showSettingsWindow:))`
- **`⌘N`** 新建便签：`MenuBarContent.swift` 显式绑定 `.keyboardShortcut("n", modifiers: [.command])`，**仅在菜单栏面板展开时命中**；面板折叠时响应链上没有这个按钮
- **`⌘Q`** 退出应用：`MenuBarContent.swift` 显式绑定 `.keyboardShortcut("q", modifiers: [.command])`，按钮内部调用 `NSApplication.shared.terminate(nil)`；**仅在菜单栏面板展开时命中**

**云端数据源重构后，进程退出不再需要 `willTerminate → flushStickiesSave`**——便签数据已是服务端权威，窗口位置由 `StickyWindowController` 的 `didMove` / `didResize` 在每次触发时同步 `frameStore.save(...)` 到 UserDefaults，`save` 方法无缓冲。
