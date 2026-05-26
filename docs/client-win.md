# Windows 客户端（client/win/）

技术栈：**C++20 + Win32 + Direct2D + DirectWrite + WinHTTP + nlohmann-json + Ninja + vcpkg**。`CMakeLists.txt` 设 `CMAKE_CXX_STANDARD 20` + `CXX_STANDARD_REQUIRED ON`；依赖清单见 `client/win/vcpkg.json`（`nlohmann-json` + `cppwinrt`；测试 feature 追加 `gtest`），vcpkg baseline 钉死在 `vcpkg-configuration.json`，**不要**随便升 baseline（升级会同时拉全所有依赖版本，连锁触发兼容问题）。

---

## 为什么是自绘 UI（不是 WPF / WinUI / Qt）

- WPF / WinUI 强依赖 .NET 运行时或 WindowsAppSDK 框架包，分发麻烦（要么装 .NET，要么走 MSIX/app attach），与"单 exe 便携版"目标不符
- Qt 虽然静态链接可做单 exe，但整体产物 >50MB 且 LGPL 合规要暴露 obj 文件，不适合 MIT 项目的轻量定位
- Win32 + Direct2D 自绘：exe 典型 2~3MB，只依赖系统 DLL（`d2d1.dll` / `dwrite.dll` / `winhttp.dll` 在 Windows 10 20H1+ 全部内置），无运行时依赖。代价是**自己画一套控件**——这也是 `ui/Controls.{h,cpp}` 存在的原因（Button / CheckBox / TextBox / Label / ScrollView 自绘，不用系统 BUTTON / EDIT）

---

## Bundle 和命名

真值来自 `client/win/CMakeLists.txt` + `src/res/app.rc` + `src/res/app.manifest`：

- **可执行名**：`stickytodo.exe`（`add_executable(stickytodo WIN32 ...)`，`WIN32` 声明让链接器生成 GUI 子系统而非控制台）
- **目标平台**：Windows 10+（`app.manifest` `<compatibility>` 段显式声明 supportedOS = Windows 10 GUID + Windows 11 GUID）。代码里**没有**显式定义 `_WIN32_WINNT`，依赖 Windows SDK / MSVC 工具链默认（SDK 10.0.x 默认 `0x0A00`=Windows 10）。如果未来要强制 20H1+ 以使用 `ID2D1DeviceContext6`，应在 `CMakeLists.txt` 显式 `target_compile_definitions(stickytodo PRIVATE _WIN32_WINNT=0x0A00 WINVER=0x0A00)`
- **产品版本号**：`src/res/app.rc` 里通过 `#define VER_MAJOR/VER_MINOR/VER_PATCH/VER_BUILD` **硬编码** `1,0,0,0`，`StringFileInfo` 的 `FileVersion` / `ProductVersion` 字符串也硬编码为 `"1.0.0.0"`——`package-win-client.sh` 不改 `.rc`，所以 exe "属性 → 详细信息"永远显示 `1.0.0.0`；外部可见的版本号只有**产物文件名**（`stickytodo-<VERSION>-windows-<x64|arm64>.zip` / `stickytodo-setup-<VERSION>-<x64|arm64>.exe`）
- **凭据存储**：**Windows Credential Manager**，通过 `wincred.h` 的 `CredWriteW/CredReadW/CredDeleteW`（封装在 `core/CredentialStore`）。**target name 按用户名动态拼接**：`L"stickytodo/" + Utf8ToWide(username)`（`MakeTargetName`）；另有常量 `kLastUserTarget = L"stickytodo/__last_user__"` 专门存"最近登录用户名"用于登录表单预填。与 macOS `KeychainStore.service = "com.hanxi.stickytodo"` 等价的本机安全区
- **本机偏好**：**HKCU\Software\stickytodo** 下的 REG_DWORD 键（见 `ui/Preferences.cpp`）。当前两个键：`skipTodoDeleteConfirm` / `skipStickyDeleteConfirm`（对齐 macOS 的 `todo.skipDeleteConfirm` / `sticky.skipDeleteConfirm` `@AppStorage` 键）
- **窗口位置**：`core/FrameStore` 存在 **`%LocalAppData%\stickytodo\frames.json`**（单 JSON 文件，不是注册表；kv 结构 `{<stickyId>: {x, y, width, height}}`）。与 macOS `UserDefaults` 的 `stickytodo.frames` 语义对等，**本机 UI 偏好、不跨端同步**。选 JSON 而非注册表：便于启动时一把读完整个 map，且用户手工编辑 / 备份更友好
- **DPI**：`src/res/app.manifest` 同时声明两个 DPI 键，遵循 MSDN per-monitor-v2 兼容写法——新 schema `<dpiAwareness ...>PerMonitorV2</dpiAwareness>` 给 Win10 1607+ 用，保留老 schema `<dpiAware ...>true/pm</dpiAware>` 作为更早版本的 fallback（portable zip 用户可能跳过 installer 的 `MinVersion=10.0.19041` 门禁）。manifest 还显式声明 `<activeCodePage>UTF-8</activeCodePage>`（Win10 1903+，让 `A` 系列 Win32 API 自动用 UTF-8，避免路径含全角时乱码）

---

## 模块职责

### App / main.cpp

`WinMain` 进程入口 + `App` 全局单例。App 持有 `HINSTANCE` / `D2DRenderer` / `AppState` / `TrayIcon` / `SettingsWindow` / 一个 `std::unordered_map<std::string, std::unique_ptr<StickyWindow>>`（stickyId → 窗口）。

**WM_STICKYTODO_* 消息路由中枢**：`PostMessageToAllStickies` 广播到所有便签窗（Refresh 用）、`PostMessageToSticky(id, msg)` 精准路由（UPSERTED/DELETED 用）。`OnStickyWindowDestroyed(stickyId)` 在 `StickyWindow::WM_DESTROY` 时把 unique_ptr 从 map 里 erase——**内存正确性的关键**：WM_STICKYTODO_STICKY_DELETED 不能在 handler 里直接 `delete this`，必须 `DestroyWindow(hwnd_)` 让 Win32 把消息泵内剩余消息处理完再触发 WM_DESTROY，再由 App 负责 erase。

### core/AppState

聚合 JWT 态、StickyNote 列表、`HttpClient`、`WebSocketClient`、`CredentialStore`、`FrameStore`、`Timer`（去抖）。

**WS 事件走"worker 线程 → PostMessageW → UI 线程"两段式路由**（`WebSocketClient::ReceiveLoop` 里的 `WinHttpWebSocketReceive` 是同步阻塞调用，其后同步触发的 `onEvent_` / `onSignal_` 是 `WebSocketClient` 自己定义的 `std::function`，不是 WinHTTP 异步回调；但仍在 worker 线程执行，UI 数据结构必须回到 UI 线程才能安全 mutate）：

- worker 线程里 `WebSocketClient` 触发 `onEvent_` / `onSignal_` → `AppState::PostWsEventToUIThread` / `PostWsSignalToUIThread` 把 `WsEvent` 堆分配后 `PostMessageW(uiThreadTarget_, WM_STICKYTODO_WS_EVENT/_SIGNAL, heap, 0)` 到 tray 的消息窗口
- tray `WndProc` 收到消息 → 调用 `AppState::HandleWsEventOnUIThread` / `HandleWsSignalOnUIThread`（UI 线程）→ 释放堆对象并分派，真实分发规则（对齐 `AppState.cpp:207-289`）：
  - `todo.*` → `PostMessageToAllStickies(WM_STICKYTODO_REFRESH)`（广播给所有便签窗；窗口去抖 300ms 后 refetch 自己关心的 TODO 列表，不相关 filter 会丢弃，与 macOS 同语义）
  - `sticky.upserted` → `MergeStickyUpserted(data)` 更新 `stickies_` 缓存 + 触发 `onStickiesChanged_`（App 订阅后调 `SyncStickyWindows()`）**同时**直接 `PostMessageToSticky(noteId, WM_STICKYTODO_STICKY_UPSERTED)` 让已存在的目标窗口重读自己的 title/bg/filter 并重绘
  - `sticky.deleted` → `MergeStickyDeleted(id)` 更新缓存 + 触发 `onStickiesChanged_`（`SyncStickyWindows` 按"windows∉stickies 关窗"幂等对账）**同时**直接 `PostMessageToSticky(id, WM_STICKYTODO_STICKY_DELETED)` 让目标窗口自行 `DestroyWindow`。**两条路径幂等但不冗余**：直接 post 是 low-latency 快路径；`SyncStickyWindows` 是 reconcile 兜底

### core/HttpClient

基于 `winhttp.dll` 的 REST 客户端；方法签名与后端一一对应，与 macOS `APIClient` / Web `api/client.ts` 契约一致。与 macOS `APIClient` 的 TodoFilterDTO 适配层对等——`codec/StickyCodec::FilterToJson` 负责 snake_case 的 JSON 字符串构造（后端约定），camelCase 的 C++ 结构体通过手写映射转换。

### core/WebSocketClient

基于 `winhttp.dll` 的 `WinHttpWebSocket*` API。与 macOS `RealtimeClient` 协议行为等价：首帧 auth（2s 服务端超时窗口内发）/ ready 帧 / 指数退避 `[1,2,4,8,16,30]s` / close code 4401 → `unauthorized` signal。

**WinHTTP API 是阻塞同步调用**，所以 `WebSocketClient` 内部跑在**独立工作线程**，事件经过 `PostMessageW` 送回 UI 线程（不能直接在 WinHTTP 工作线程里动 `AppState`——AppState 所有 mutator 假设 UI 单线程）。

### core/CredentialStore

封装 `CredWriteW/CredReadW/CredDeleteW`，屏蔽 `CREDENTIAL` 结构体的繁琐填充和 UTF-16 转换。JWT 和 username 走**两个独立的 Credential Manager target**：①`L"stickytodo/" + username`（`MakeTargetName`）存 per-user 的 JWT blob ②`L"stickytodo/__last_user__"`（`kLastUserTarget`）存 last-user。**不使用** HKCU 注册表存 last-user，保持"敏感数据全部集中在 Credential Manager"的单一职责。

### core/FrameStore

便签窗口位置**纯本机**持久化，与 macOS `FrameStore` 语义对等。

**写路径**：`StickyWindow::OnResize`（`StickyWindow.cpp:250-259`，WM_SIZE 分派）和 `StickyWindow::OnMove`（`:261-263`，WM_MOVE 分派）内部都调 `SaveFramePosition()`（`:265` 起），最终落到 `FrameStore::Save(stickyId_, frame)`（`:277`）。`Save` / `Remove` / `PruneOrphans` 三个公开写接口都通过内部 `PersistAll(map)` helper 做"读全量 → 改内存 map → 写回整个 `frames.json`"的原子 flush（`FrameStore.cpp:69-99`），**无 in-memory 缓冲层**，每次写操作都立即 I/O 一次（文件小、写频率低，不必引入 coalescing）。

**读路径**：`bool StickyWindow::Create()`（`:103-148`）在 `CreateWindowExW` 前先给 `x/y/w/h` 打**二分默认值**——位置 `x = y = CW_USEDEFAULT`（Win32 让 OS 自己级联摆放首次开的便签；与 macOS 对等但落点不同，macOS 由调用方拿 `defaultFrame + 偏移`），尺寸 `w = Theme::kStickyDefaultWidth, h = Theme::kStickyDefaultHeight`；随后调 `FrameStore::Load(stickyId_)`——签名 `std::optional<FrameRect>`，缺失返回 `nullopt`，调用处 `if (frame.has_value()) { ... }` 覆盖默认值。

### codec/JsonHelper

`nlohmann::json ↔ 各 POD 模型` 的手写转换器（见 `JsonHelper.h`）：**Todo** (`ParseTodo` / `ParseTodos` / `TodoToJson`) / **StickyNote** (`ParseStickyNote` / `ParseStickyNotes` / `StickyNoteToJson`——**整个便签的序列化归属在 JsonHelper 而非 StickyCodec**，StickyCodec 只处理便签内嵌的 `bg_color` 和 `filter` 两个子字段) / **Filter** (`ParseFilter` / `FilterToJson`——与 StickyCodec 的 `FilterToJson/JsonToFilter` 并存) / **AuditLog** (`ParseAuditLog` / `ParseAuditLogs`) / 通用 `SafeGet*` helpers。

容错策略"脏数据兜底"——缺失字段用默认值，类型不匹配走 `SafeGet*` 的 `defaultVal` 兜底而不抛异常（与 Web `stickyCodec.ts` 的兜底哲学一致）。

### codec/StickyCodec

便签内嵌**两组** JSON 子字段的双向转换：

- **`bg_color` 组（4 个方法）**：`HexToBgColorJson(hex)` / `BgColorJsonToHex(json)` 做 hex 字符串（如 `"#FFEB8A"`）↔ 后端 `CodableRGBA` JSON（`{"red":1.0,"green":0.92,"blue":0.54,"alpha":1.0}`，浮点 0-1）；更底层的 `ParseBgColor(json) → RgbaColor` 和 `RgbaToJson(RgbaColor) → json` 直接吐 / 吃 `RgbaColor` POD（`StickyCodec.h:14-25`，含两个便利方法：`uint32_t ToColorRef() const` 转 Win32 `COLORREF`（alpha 丢弃）；`void ToD2DColor(float& r, float& g, float& b, float& a) const` 通过 out-param 吐 `[0,1]` 浮点）
- **`filter` 组（2 个方法）**：`FilterToJson(Filter)` / `JsonToFilter(str)`：`models::Filter` POD ↔ snake_case JSON 字符串。**`StickyCodec::FilterToJson` 是全仓唯一的 filter 序列化调用点**（`StickyWindow.cpp:1301` 保存筛选条件时调用），`JsonHelper::FilterToJson`（`JsonHelper.cpp:170`）定义存在但**当前无调用者**——属于跨 codec 边界的遗留重复实现，新代码走 StickyCodec 版本

**字段命名"r/g/b/a" vs "red/green/blue/alpha"**：三端实际用的是**全拼** `red/green/blue/alpha`（macOS `APIClient.swift:398` + `StickyNote.swift:146` 的 `CodableRGBA(red:green:blue:alpha:)`、Windows `StickyCodec.h:11-13` 注释、Web `client/web/src/lib/stickyCodec.ts` 的 `hexToBgColorJSON`/`bgColorJSONToHex` 全是全拼），不是单字母缩写。**后端 Go 侧不对 `{"red":...}` 做字段级反序列化**（`server/internal/handler/sticky_handler.go:44` 的 DTO 和 `server/internal/model/models.go:73` 的 GORM 模型都把 `BgColor` 声明为 `string` 类型，透明字符串存储），但 `models.go:73` 注释 `// JSON: {red,green,blue,alpha}` 是**文档级契约**。字段名一致性的**代码级契约**由三个客户端的 codec 共同维护。

### ui/D2DRenderer

`ID2D1Factory` / `IDWriteFactory` / `ID2D1HwndRenderTarget` 单例管理；所有 `StickyWindow` / `SettingsWindow` / `FilterEditor` 共享同一个 factory（COM 引用计数安全）。

### ui/Theme

与 macOS `Color.*` Swift 常量逐条对应的 D2D `D2D1_COLOR_F` 函数（`TextPrimary()` / `CheckboxFill()` / `ButtonHover()` 等）；不用 static 全局是因为 D2D 颜色结构体在 header-only 初始化会和 MSVC `/ZI`（Edit and Continue）冲突。

### ui/Controls

自绘控件库。`Button` / `CheckBox` / `TextBox` / `Label` / `ScrollView`。**每个控件三段式接口**：`rect`（布局）+ `Draw(rt, dw, dpi)`（渲染）+ `HandleMouse(msg, x, y[, dpi, dw])` 或 `HandleChar(c)` / `HandleKey(vk)`（输入）。`Button` 额外带 `selected: bool` 字段（用于分段选择器 / tab 等"持久选中"场景，独立于 Normal/Hover/Pressed 的 state 机），`CheckBox::Draw` 在 `!enabled` 时按 0.5 alpha 降透明度。

#### TextBox 契约

（和 macOS `NSTextField` / Win32 `EDIT` 控件语义保持一致，但全部自己画）

- **签名特殊**：`TextBox::HandleMouse(msg, mx, my, dpi, dw)` 比其他控件多两个参数（`dpi` + `IDWriteFactory*`），因为字符 hit-test 必须和 Draw 用同一套 DirectWrite 字体度量才不错位。所有调用方（`SettingsWindow::On*` / `FilterEditor::On*` / `StickyWindow::On*`）都已升级到这个签名；Button / CheckBox / ScrollView 仍是 3 参数不动
- **selection / 剪贴板能力**：拖拽选中（LBUTTONDOWN 锚点 → MOUSEMOVE 扩展 → LBUTTONUP 结束）、Shift+箭头/Home/End 扩展选区、Ctrl+Left/Right 按单词跳（`iswspace` 判边界）、Ctrl+A 全选、Ctrl+C/X/V 走 `CF_UNICODETEXT` 剪贴板（`OpenClipboard(nullptr)` 合法）、Backspace/Delete 在有选区时整段删
- **isPassword=true 特殊**：Ctrl+C / Ctrl+X 短路 return true 不执行（防止 mask 字符泄漏到剪贴板），但其他选中 / 删除仍可用
- **选中背景绘制**：`Theme::CheckboxFill()` alpha=0.30；放在文字 DrawText 之前；用同一个 `IDWriteTextLayout::HitTestTextPosition(selStart) / (selEnd)` 算像素 X，保证"选中边界"和"caret 位置"像素级对齐
- **SetCapture / ReleaseCapture 契约**：TextBox 自身不调 `SetCapture` —— 必须由宿主窗口在 `WM_LBUTTONDOWN` 里当 `HandleMouse` 返回 true 时调 `SetCapture(hwnd_)`，在 `WM_LBUTTONUP` 里 `if (GetCapture()==hwnd_) ReleaseCapture()`。不 SetCapture 的后果：用户拖拽选中时鼠标一离开输入框 rect，MOUSEMOVE 就不再送到我们窗口，rubberband 选区会冻结
- **StickyWindow 的坐标系特殊**：`draftBox_` / `editBox_` 的 `rect.y` 是**内容坐标系**（`ScrollView::BeginContent` 已平移过 `-scrollOffset`），所以宿主 `HandleMouse` 转发时必须用 `contentFy = fy + scrollView_.scrollOffset` 而非 `fy`
- **字符 hit-test 实现**：`TextBox::HitTestCharIndex(mx, dpi, dw)` 用和 Draw 完全一致的 `MakeTextBoxFormat(dw, dpi)` 建临时 `IDWriteTextLayout`，`HitTestPoint(localX, h/2, &isTrailing, ...)` 做"四舍五入到最近字符边界"
- **wstring → string 必须走 WideToUtf8 helper**（当前实现在 `SettingsWindow.cpp` 的 anonymous namespace）：绝对禁止 `std::string s(ws.begin(), ws.end())` 反模式 —— 会静默截断 wchar_t 到 char。若第三处也需要 UTF-16↔UTF-8 转换，应该把 helper 提升到 `core/StringUtils.h` 共享

### ui/Preferences

封装 HKCU 偏好读写，当前提供 `ShouldSkipTodoDeleteConfirm` / `SetSkipTodoDeleteConfirm` / `ShouldSkipStickyDeleteConfirm` / `SetSkipStickyDeleteConfirm` 四个函数。**任何新增的"记住用户选择"偏好都应加到这个文件**，不要在各窗口 cpp 里埋匿名 namespace 的 Read/Write 辅助函数。

### ui/StickyWindow

单个便签窗口。`styleMask = WS_POPUP | WS_THICKFRAME`（无系统标题栏、可调尺寸），`SetWindowLongPtrW(GWL_EXSTYLE, ... | WS_EX_TOOLWINDOW | WS_EX_TOPMOST)` 置顶且不在任务栏显示。内部渲染分 4 区：

- **标题栏**（`titleBarHeight_` 高，自绘）：临时布局的 `Label titleLabel`（只读文本，显示 `stickyNote_.title`；**当前实现没有点击进入重命名态的交互**——便签标题修改仅通过服务端改动 + WS `sticky.upserted` 事件回灌） + `closeButton_`（`×` U+00D7，`rect = {W-32, 4, 24, 24}`，点击 → `App::CloseStickyWindow(stickyId_)`，仅关闭窗口，不删除便签数据） + `settingsButton_`（`⚙` U+2699，点击 → `App::ShowSettings()`，永远可见） + `plusButton_`（`+` U+002B，点击 → `BeginDraft()`） + `trashButton_`（`🗑` **U+1F5D1 WASTEBASKET emoji**——**故意选 emoji** 而非 Segoe UI Symbol，与下文 TodoRow 的 `✖` 取舍相反；权衡理由：trashButton 是**hover-only** 组件，即便老版 Windows 字体 fallback 成 tofu，hover 信号已独立传达"破坏性操作"意图，见 `StickyWindow.cpp:379-385` 注释；**仅** `titleBarHovered_ == true` 时渲染且 rect 非零，否则置 `{0,0,0,0}` 使 hit-test 失败避免误触；点击 → `DoDeleteSticky()`）
- **DraftTodoRow**：顶部待办新增入口（Enter 提交、Esc 取消）
- **TodoRow 列表**：每行 hover 显示三个行动按钮（`RowHitTest::Zone` 枚举）。**三个 slot 的图标都刻意选 Segoe UI Symbol 内置字符（BMP 内码点）而非 emoji**——TODO 行是持续可见 UI，若字体 fallback 成 tofu 会非常显眼（`StickyWindow.cpp:536-538` 注释）。**全部三个 slot 都受 `!todo.IsDeleted()` 守卫**：
  - `ActionComplete` — 非软删时绘制，未完成 → `✓` (U+2713)，已完成 → `↺` (U+21BA)；handler：`IsDone() → DoReopen` / else → `DoComplete`
  - `ActionEdit` — 非软删时绘制，固定 `✏` (**U+270F PENCIL**，**不是** U+270E LOWER RIGHT PENCIL——后者在老 Win10 Segoe UI 上 fallback 成 tofu，见 `:525-530` 注释)；handler：`BeginTitleEdit(rowIndex)`
  - `ActionDelete` — **三个 slot 中唯一对软删态也绘制的**：非软删 → `✖` (U+2716) 触发软删，软删态 → `↶` (U+21B6) 触发恢复
  - **点击 TODO 标题文本**（`RowHitTest::Zone::Title`）等价于点 `ActionEdit`，同样受 `!todo.IsDeleted()` 守卫
- **FilterBar**（底部）：`filterButton_` 文本由 `BuildFilterSummary(filter_)` 动态构造，点击弹出 FilterEditor 模态
- **WM_STICKYTODO_STICKY_DELETED 必须通过 `DestroyWindow(hwnd_)` 走常规消息泵路径，不要直接 delete**

### ui/SettingsWindow

设置窗口。**3 个 Tab** 对齐 macOS `SettingsView`：

- 「设置」Tab：Base URL / 登录表单 / 测试连接 / 登出 / 通用偏好（两个删除确认 CheckBox）
- 「历史」Tab：登录后拉取全局审计日志；未登录占位提示
- 「关于」Tab：品牌 / 版本 / 项目链接

### ui/FilterEditor

模态筛选编辑器。与 macOS `FilterEditor.swift` 1:1 对齐：状态分段选择器（全部/未完成/已完成）、标签/关键词 TextBox、软删 2 CheckBox（`onlyDeleted` → `includeDeleted.enabled=false` 即时视觉联动）、页大小 stepper（10-200，step 10）、取消/重置/保存头部按钮。

**Win32 模态实现**：`CreateWindowExW(WS_VISIBLE)` + `EnableWindow(owner, FALSE)` + 局部 `GetMessage` 循环直到窗口销毁 + 返回前 `EnableWindow(owner, TRUE) + SetFocus(owner)`（MSDN 定义的"模态消息循环"模式；不用 `DialogBox` 是因为渲染路径是 Direct2D 自绘）。

### ui/TrayIcon

`Shell_NotifyIconW` 封装。图标菜单按鉴权态分化（`AppState::IsAuthenticated()` 判定）：

- **未登录**：`Settings` / `Quit`
- **登录后**：`New Sticky Note` / `Settings` / 分隔符 / `Logout` / `Quit`
- 所有菜单项文案当前都是英文字面量，与 `SettingsWindow` 的 Tab 名（`Settings` / `History` / `About`）保持一致
- 交互：`WM_RBUTTONUP` / `WM_CONTEXTMENU` → `ShowContextMenu`；`WM_LBUTTONDBLCLK` → `App::ShowSettings()`；左键单击**无行为**
- 当前图标**未使用** `NIF_GUID`，仅靠 `(hwnd, uID)` 定位；若未来要让 Windows 升级版本后仍记住"图标已置顶"偏好，需要给它加 process-wide 恒定 GUID 并在 `NOTIFYICONDATAW.uFlags` 开 `NIF_GUID`

---

## 模型（`src/models/`）

共 4 个，与后端 `models.go` + `types/api.ts` + Swift `Models/` 一一对应：`Todo.h` / `AuditLog.h` / `StickyNote.h` / `Filter.h`。**全部 POD（无虚函数、无继承）**，JSON 序列化通过 `codec/JsonHelper` + `codec/StickyCodec` 的手写函数——没用 `NLOHMANN_DEFINE_TYPE_INTRUSIVE` 是为了完全掌控字段缺失 / 类型不匹配时的兜底策略（宏版本遇到字段缺失会抛异常，我们要的是容错 → 默认值）。

---

## 快捷键

- Windows 桌面没有像 macOS `⌘,` 一样的系统级"打开设置"约定，SettingsWindow 的入口有 3 条：①**托盘图标右键菜单** → `Settings` ②**托盘图标双击**（等价于右键 → `Settings`，`TrayIcon::WndProc` 的 `WM_LBUTTONDBLCLK` 分派到 `App::ShowSettings`）③**便签窗口标题栏 `⚙` 按钮**（`settingsButton_` 永远可见）
- 便签窗口内部：`Enter`（DraftTodoRow 聚焦时）= 提交新 TODO；`Esc`（编辑中）= 取消；**没有任何 `RegisterHotKey` 调用的全局/系统级快捷键**（grep `RegisterHotKey` / `MOD_CONTROL` / `VK_N` 全仓返回空；与 macOS `⌘N` 仅菜单展开时可触达是同一哲学）

---

## 网络调用异步化

所有 UI 线程触发的 HTTP 调用都走 `HttpClient::Async*`，**严禁**在 UI 线程同步调 `HttpClient::*`：

- **问题背景**：WinHTTP 的 `WinHttpSendRequest` / `WinHttpReceiveResponse` 是同步阻塞的，且默认超时**非常宽松**（按 Microsoft 文档：`dwResolveTimeout = 0` 表示"无超时/infinite"而非 0 秒，`dwConnectTimeout = 60 s`、`dwSendTimeout = 30 s`、`dwReceiveTimeout = 30 s`；累加最坏情况是**无限**，即使名字解析快也至少 **120 s**）。在 `Button::onClick` lambda 里同步调 HTTP 会冻结窗口消息泵，表现为 Windows 弹出"未响应"灰屏。macOS 侧同样场景靠 `URLSession` 的 completion handler 天然异步
- **超时值**：`HttpClient::DoRequest` 统一 `WinHttpSetTimeouts(session, 10000, 10000, 10000, 10000)` — resolve / connect / send / receive 各 10 s，兼顾内网快响和公网弱网。**会话级默认**，单个请求不再单独覆盖
- **UI-thread marshal 机制**：`core/UIThreadMarshal.{h,cpp}` 提供 `SetUIThreadTarget(HWND)` / `PostToUIThread(std::function<void()>)`。实现：把 lambda heap-allocate 后通过 `PostMessageW(target, WM_STICKYTODO_RUN_ON_UI, 0, (LPARAM)funcPtr)` 投递；`TrayIcon::WndProc` 收到 `WM_STICKYTODO_RUN_ON_UI` 时 `invoke() + delete`。**target HWND 选 tray**（不是 sticky / settings 的 HWND），因为 tray 是 App 生命周期内存活最长且唯一的窗口
- **HttpClient Async API 形状**：每个同步方法都有配对的 `Async*` 变体，callback 签名是 `std::function<void(Result)>`。内部样板：`std::thread([...]{ auto r = Sync版本(...); PostToUIThread([cb, r]{ cb(r); }); }).detach()`。**故意不用 `std::future` / `std::promise`**——MSVC `std::promise<void>` 在 WinHTTP 错误分支里存在生命周期陷阱（worker 抛 `future_error` 比 UI 线程读 future 更早时崩溃），`std::function` 回调简单可靠
- **Worker 线程 `detach()` 的代价**：进程退出时最多有 **~40 s**（4 × 10 s 超时预算）的 detached worker 残留。`App::Shutdown` 闭环吸收：
  1. step 2 `SetUIThreadTarget(nullptr)` — 后续 `PostToUIThread` 返回 false 直接丢弃 callback
  2. step 3 `drain` 把 tray HWND 消息队列里已排队的 `WM_STICKYTODO_RUN_ON_UI` 全部 `DispatchMessage`（lambda 执行 + 堆对象释放，防止内存泄漏）
  3. step 4-8 按 tray → settings → stickies → state_ → D2D 的顺序 reset
  4. 最后 `g_app = nullptr`（`App.cpp:224`）。detached worker 即使醒来也只会读到 `g_app == nullptr` early-return，进程被 `ExitProcess` 清理，不 join 是**刻意选择**

### UI 回调的 `this` 捕获安全性

三种 guard 模式按窗口类型区分，**任何新加的异步回调必须选其一**：

- **StickyWindow（窗口数量不定、生命周期短、可被 WS 推送销毁）**：`std::shared_ptr<std::atomic<bool>> alive_` 字段（`StickyWindow.h:200`），构造时默认 true、析构函数**第一行**置 false。回调 capture `[this, alive = alive_, ...]`，入口先 `if (!alive->load()) return;`。shared_ptr 保证 atomic 的存储在回调执行前不会被释放。二次守卫 `if (!hwnd_) return;` 作为 defense-in-depth
- **StickyWindow LoadData 特化 — 请求代 token**：`loadDataGeneration_` 单调计数器，`LoadData()` 入口 `uint64_t myGen = ++loadDataGeneration_`，callback capture `myGen` 并在 alive 守卫后判 `if (myGen != loadDataGeneration_) return;`。**必要性**：`ShowFilterEditor` 失败回滚路径会触发第二次 `LoadData()`，如果没有代号，两次 `AsyncListTodos`（不同 filter）的 callback 可能**乱序落回**
- **SettingsWindow（单例，App 持有 `unique_ptr`）**：guard `auto* app2 = GetApp(); if (!app2 || app2->GetSettingsWindow() != self) return;`
- **TrayIcon（单例，菜单命令触发但无 `this` 依赖）**：NEW_STICKY 的 `UpsertStickyAsync` callback **不捕获 `this`**，只捕获值类型数据 + 通过 `stickytodo::GetApp()` 重查拿 state

### 乐观 vs 悲观的分工

回答"为什么 StickyWindow 10 处调用不是一刀切同一策略"：

- **写操作（CreateTodo / UpdateTodo / Complete / Reopen / Restore / Delete / UpsertSticky）→ 乐观 + 回滚**：用户点击后立即本地改 `todos_` / `filter_` 并重绘，HTTP 异步飞；callback 成功用服务端返回的 Todo 覆盖占位行，callback 失败用 snapshot 回滚。CreateTodo 额外用 `nextPendingTodoId_ = UINT64_MAX` 递减作为占位 ID（服务端真实 ID 小，不冲突）
- **读操作（ListTodos × 2）→ 悲观 Loading**：`todosLoading_ = true` 触发 DrawTodoList 显示 `Loading...` 占位（**仅当 `todos_` 为空**，后续 refresh 不闪屏只显示底部状态）
- **DeleteSticky → 悲观 + 按钮禁用**：`stickyDeleting_ = true` 禁用 trashButton，HTTP 异步飞；成功直接 `PostMessageW(WM_STICKYTODO_STICKY_DELETED)` 走正常关窗路径（WS 广播兜底），失败 flip 回 false。**不能乐观**——"关窗"本身就是最终操作，关了就没有 UI 表达错误的地方

**SettingsWindow 的 inFlight 字段**：`testInFlight_` / `loginInFlight_` / `auditInFlight_` 三个 bool。按钮 `enabled = !xxxInFlight_`，callback 无论成败都 flip 回 false。比 macOS 侧的 `isLoading` 语义等价，但没有 `@Published` binding，手动 `InvalidateRect` 触发重绘。

---

## 与 macOS 必须对齐的不变量

回归测试时优先看这几条：

1. **乐观追加**：`DraftTodoRow` 提交成功后 `todos_.push_back(new_todo)`，立即重绘，不等 WS → 与 macOS `TodoListViewModel.commitDraft` 语义一致。Windows 侧具体路径：`CommitDraft` 立即把占位 Todo 推入 `todos_`（占位 ID = `nextPendingTodoId_--` 从 `UINT64_MAX` 递减），然后发 `AsyncCreateTodo`；callback 成功用服务端 Todo 替换占位行，失败移除占位行
2. **乐观删除**：`StickyWindow::DoDelete(rowIndex)` 先弹三选一确认框（受 `ShouldSkipTodoDeleteConfirm` 短路），用户确认后**立即**把 `todos_[rowIndex].deleted_at = "pending"`（本地软删视觉占位，UI 立刻显示恢复按钮），然后发 `AsyncDeleteTodo(todoId)`；失败用 `prevDeletedAt` 快照回滚。WS `todo.deleted` 到达后 refetch 对齐真正的服务端时间戳
3. **删除确认 "N/Y/Cancel" 三选一**：`IDYES = 直接删` / `IDNO = 删除并不再提示（写 HKCU）` / `IDCANCEL = 放弃`——与 macOS 的 3-way `alert` 三按钮形态对齐
4. **Frame 不跨端**：`UpsertSticky` 请求体里 `frame` 字段恒 `"{}"`，frame 只走 `FrameStore` 本机持久化

---

## DPI 布局契约（PerMonitorV2）

`app.manifest` 声明 `PerMonitorV2` + `main.cpp` 调 `SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)`。这决定了**整个客户端的坐标空间都是物理像素**（不是 96-DPI 虚拟像素）：

- `GetClientRect` / `GetWindowRect` 返回物理像素
- `WM_MOUSEMOVE` / `WM_LBUTTON*` 的 `GET_X_LPARAM(lParam)` 是物理像素
- `WM_DPICHANGED` lParam 里的 suggested rect 已经是新 DPI 的物理像素

因此**所有 rect 摆放、hit-test 比较、字体尺寸都必须用物理像素**。

### 强制约定

0. **`D2DRenderer::CreateRenderTarget` 必须显式设 `rtProps.dpiX = dpiY = 96.0f`**（这是一切其他 DPI 约定的大前提）。`D2D1::RenderTargetProperties()` 的默认 DPI 是 **0**，在 D2D 内部被解释成"desktop DPI" — 也就是 D2D 自己会把我们传入的坐标乘以 `desktopDpi/96`。我们的代码已经手动把所有 rect × dpi，如果 D2D 再乘一次就会变成 **1.5 × 1.5 = 2.25×** 双重缩放（控件比窗口大、文字溢出、看得到但点不到，因为 `WM_MOUSE*` 的物理像素坐标没经过 D2D 矩阵）。**固定用法**：`D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f)`
1. **`Theme::kXxx` 是 96-DPI 基准值**（`kPadding = 8`, `kTodoRowHeight = 36`, `kButtonHeight = 32`, `kCheckboxSize = 18`, `kCornerRadius = 6`, `kScrollbarWidth = 10`, `kInputHeight = 28` 等）。**使用处一律 `× dpi`**，禁止裸用
2. **字面 float 像素值也是 96-DPI 基准**（`4.0f`, `8.0f`, `22.0f`, `24.0f`, `32.0f`, `40.0f` 等出现在 rect / 坐标 / 线宽 / 圆角里的数字）。**使用处一律 `× dpi`**。alpha / 比例因子（`0.5f` alpha、`0.3f` 勾线相对尺寸、`2.0f` 滚动速度比例）**不是 px，不要乘**
3. **`dpi` 来源**：`D2DRenderer::GetDpiScale(hwnd) = GetDpiForWindow(hwnd) / 96.0f`。OnPaint 开头取一次，传到所有 Draw 函数参数里
4. **Controls::Draw 内部硬编码 px 也必须 × dpi**：TextBox 的 8px inset / 4px 圆角 / 1px 边框 / 0.5px caret 偏移；CheckBox 的 kCheckboxSize / 3px 圆角 / 1.5px/2px 描边 / 6px 标签间距；Button 的 kCornerRadius；ScrollView 的 kScrollbarWidth / 20px 最小滑块 / 40px 滚轮步长 / 1px 滑块 inset / 3px 圆角。**唯一例外**是字体 — `CreateTextFormat` 的 fontSize 传 `fontSize * dpi`，DirectWrite 的布局引擎本身按物理像素渲染
5. **ScrollView 特殊处理**：因为 `HandleMouse` / `HandleWheel` / `DrawScrollbar` / `GetContentClipRect` 没有 `dpi` 参数，ScrollView 有一个 `float dpi = 1.0f` 成员。**使用方必须在 OnPaint 开头赋值**（`scrollView_.dpi = dpi;` / `historyScroll_.dpi = dpi;`），否则滚动条宽度 / 滚轮速度 / 最小滑块高度会以 1.0 渲染
6. **`StickyWindow::titleBarHeight_` / `filterBarHeight_` 是物理像素缓存**，由 `RefreshLayoutMetrics()` 在每次 OnPaint 开头刷新。OnNcHitTest / OnMouseMove / OnLButtonDown 直接读缓存，**不要再乘 dpi**。其他 Theme::k* 读取处仍需 × dpi
7. **`ComputeRowLayout(width, rowTop, dpi)` 必须传 dpi**，返回的 `RowLayout` 所有字段（含新增的 `checkboxSize`/`actionIconSize`/`actionGap`）都是物理像素。`HitTestRow(..., dpi, todos)` 同样接 dpi
8. **窗口创建尺寸必须 × dpi**：`SettingsWindow::Create` / `FilterEditor::ShowModal` 里 `CreateWindowExW` 的宽高参数是物理像素，直接传 `kXxxWidth`（96-DPI）会得到一个只覆盖左上角 `1/dpi²` 面积的"小窗"。固定模式：`GetDpiForWindow(owner)` 或 `GetDpiForSystem()` → `createScale = dpi/96` → `scaledW = kXxxWidth * createScale`
9. **`WM_DPICHANGED` 必须响应**：SettingsWindow / StickyWindow / FilterEditor 三个 WndProc 都接了。标准处理 = 采纳 lParam 的建议 rect（`SetWindowPos(..., SWP_NOZORDER | SWP_NOACTIVATE)`）+ `InvalidateRect`。StickyWindow 额外调 `RefreshLayoutMetrics()` + `SaveFramePosition()`。**TrayIcon 是 message-only window，不需要处理 WM_DPICHANGED**
10. **TrayIcon 图标 DPI 感知**：用 `LoadImageW(IDI_APPICON, IMAGE_ICON, GetSystemMetricsForDpi(SM_CXSMICON, sysDpi), ..., LR_DEFAULTCOLOR | LR_SHARED)`，**不是** `LoadIconW`（后者强制加载 SM_CXICON = 32px 基准，托盘区再被双线性缩成 16/20/24px 会糊）。`LR_SHARED` 让系统管生命周期，**不要调 `DestroyIcon`**（MSDN: "Do not use this function to destroy a shared icon"）

### 调试 DPI 问题的快速清单

按出现概率排序：

| 症状 | 根因定位 |
|---|---|
| **首屏控件约 2.25× 放大、文字溢出、看得到点不到** | **D2D render target 的 dpiX/dpiY 没显式设为 96.0f** — 最常见的隐式缩放点 |
| 按钮可见但点不到 | hit-test 侧的 rect / 阈值没 × dpi，与 Draw 的 rect 错位 |
| 窗口尺寸对但内容挤在左上角 | 窗口 create 时 kXxxWidth 没 × createScale |
| 控件间距过密 / 文字压在一起 | y 步进字面值（`y += 22.0f`）没 × dpi |
| 整个窗口"小一圈" | 某个 Draw 函数漏给 rect × dpi |
| 第一次渲染正常，移动到另一屏显示器后错乱 | WM_DPICHANGED 没处理，或处理了但没触发全量重绘 |
| 滚动条点不中 / 滚轮巨慢 | ScrollView::dpi 没被使用方赋值，默认 1.0 |
| 托盘图标模糊 | 还在用 `LoadIconW` 而非 `LoadImageW + SM_CXSMICON` |

---

## vcpkg / WinHTTP / Inno Setup 纪律

### vcpkg manifest 模式

依赖全写在 `client/win/vcpkg.json`，**不要**用 classic 模式的 `vcpkg install xxx`（会污染全局）。当前 top-level `dependencies`：

- `nlohmann-json`（运行期 header-only JSON 库，被 `JsonHelper` / `StickyCodec` 使用）
- `cppwinrt`（**当前代码尚未使用**——grep `client/win/src` 没有任何 `#include <winrt/...>` 或 `cppwinrt.exe` 自定义构建规则。保留它和 `CMakeLists.txt` 里的 `windowsapp` import lib 是为**未来引入 C++/WinRT API 做预留**——比如接入 Windows Toast Notifications / ApplicationData 时可以直接 `#include <winrt/Windows.UI.Notifications.h>` 而无需改 `vcpkg.json`。不增加 exe 体积 / 启动开销 — header-only 不被 `#include` 就不产生代码；`windowsapp.lib` 是 umbrella import lib，链接器对未引用符号不做任何处理）

`features.tests.dependencies` 只有 `gtest`，由 `debug` preset 的 `VCPKG_MANIFEST_FEATURES=tests` 激活，release 构建**不**拉 gtest。

加新依赖时：①改 `vcpkg.json` 的 `dependencies` 数组 ②如果是仅测试用依赖，放到 `features.tests.dependencies` 下 ③`cmake --preset` 时 vcpkg 自动拉取、无需手动 install。

### WinHTTP WebSocket 协议纪律

呼应 [server.md WS 契约](server.md#websocket-协议契约-apiws)：

- `/api/ws` 契约规定"除首帧 auth 外不接受任何上行业务帧 → close 4400"。`WebSocketClient.cpp` 的 `ReceiveLoop` **绝对不要**发业务/应用层 ping（会被服务端当作违规上行帧立刻 close 4400）。保活靠服务端 30s WS ping，WinHTTP 会**自动**回 pong，无需应用层做任何事
- `WinHttpWebSocketReceive` 是阻塞调用。想让 `Disconnect()` 能立即唤醒它的唯一可靠办法是**从另一线程关闭 handle**——这就是 `liveWebSocket_` `std::atomic<void*>` 的用途：`Disconnect` `exchange(nullptr)` 拿到 handle、调 `WinHttpCloseHandle`，receive 立即返回 `ERROR_WINHTTP_OPERATION_CANCELLED`，worker 线程检测到 `shouldRun_ == false` 直接 break。**不要**试图用 `WinHttpSetTimeouts` + 短超时轮询实现"伪阻塞"，会退化成 busy loop
- WS 回调（`onEvent_` / `onSignal_`）在 worker 线程执行，`AppState` 订阅的 lambda **必须**把事件 marshall 回 UI 线程（当前实现：`PostWsEventToUIThread` / `PostWsSignalToUIThread` → `WM_STICKYTODO_WS_EVENT` → `Tray::WndProc` → `AppState::HandleWsEventOnUIThread`）。新加回调时不要直接在 lambda 里访问 `stickies_` / `HWND` 等 UI 线程拥有的数据，否则触发 UB

### winhttp.lib 的特殊声明位置

**`winhttp.lib` 是全仓库唯一不在 `CMakeLists.txt` 的 `target_link_libraries` 里的系统库**：它只通过 `WebSocketClient.cpp` 和 `HttpClient.cpp` 两个文件顶部**各自**的 `#pragma comment(lib, "winhttp.lib")` 就地声明。对比而言，`CMakeLists.txt` 当前 `target_link_libraries` 里列的系统库（`d2d1` / `dwrite` / `dxgi` / `windowscodecs` / `credui` / `advapi32` / `shell32` / `ws2_32` / `ole32` / `uuid` / `windowsapp`）中，只有一部分在对应源文件里**额外**叠加了 pragma——`D2DRenderer.cpp` 叠加 `d2d1.lib` / `dwrite.lib`，`TrayIcon.cpp` 叠加 `shell32.lib` / `ole32.lib`，`CredentialStore.cpp` 叠加 `advapi32.lib`（双声明属于冗余保险，MSVC 链接器按符号去重，不冲突）；其余的 `dxgi` / `windowscodecs` / `credui` / `ws2_32` / `uuid` / `windowsapp` **只在 CMake 里单点声明**。

这意味着**winhttp 是唯一 pragma-only** + **少数库是 CMake+pragma 双声明** + **多数库只在 CMake 单点声明**三种口径并存，增减系统库时要同时 `grep -rn "#pragma comment(lib" client/win/src` + 对照 CMakeLists 的 `target_link_libraries`。

### UI 线程模型

- Windows 客户端只有一个 UI 线程（主线程），所有窗口过程（`StickyWindow::WndProc` / `SettingsWindow::WndProc` / `FilterEditor::WndProc` / `TrayIcon::WndProc`）都在它上面运行
- WS worker 线程通过 `PostMessageW(uiThreadTarget_, WM_STICKYTODO_WS_EVENT, ...)` 发消息——`uiThreadTarget_` 是 tray 的隐藏消息窗口（tray 最早创建，生命周期覆盖整个 App 运行期）
- 便签窗口间的广播通过 `AppState::PostMessageToAllStickies(WM_STICKYTODO_REFRESH)`——每个 StickyWindow 收到后调 `RefreshFromState()` 重新从 `AppState` 拉数据 + Invalidate

### Inno Setup 脚本（`installer/setup.iss`）

- `AppId` 固定 GUID `{{4B5B6C2E-9E7B-4F3D-A8C5-0D6A1B2C3D4E}}`（frozen，**两架构共用**）。**绝不要**每次发版生成新 GUID、**也不要**给 arm64 单独换 GUID——AppId 是 Windows "已安装应用"列表里判定"升级 or 并存"的主键，改了就意味着老版本不会被自动覆盖、会并存两份；两架构共 GUID 的刻意设计让同一主机上 x64 → arm64 切换能走升级路径
- 当前安装行为：`PrivilegesRequired=lowest` + `DefaultDirName={autopf}\StickyTodo` + `PrivilegesRequiredOverridesAllowed=dialog commandline`。组合效果：默认不弹 UAC，走 per-user 模式（`{autopf}` 解析成 `{userpf}` = `%LocalAppData%\Programs\StickyTodo`）；用户也能在安装向导里显式勾选"为所有用户安装"，此时弹 UAC，`{autopf}` 解析成 `{commonpf}` = `%ProgramFiles%\StickyTodo`。**不要**误以为 `DefaultDirName` 写死 `{localappdata}\Programs\StickyTodo` 才是 per-user——那样会丢掉 per-machine 升级路径
- **架构 gating**（`/DAppArch=<x64|arm64>`）：x64 版 `ArchitecturesAllowed=x64compatible` + `ArchitecturesInstallIn64BitMode=x64compatible`（允许 native x64 + Win11-arm64 的 x64 emulation 作为 fallback；`x64compatible` 关键字是 Inno Setup 6.3+ 引入的专用语义）；arm64 版 `ArchitecturesAllowed=arm64` + `ArchitecturesInstallIn64BitMode=arm64`（**仅** native arm64，不接受 emulation）。`ArchitecturesInstallIn64BitMode` 还会把 `{autopf}` / 注册表视图切到 64-bit 变体，否则 64-bit exe 会错误地落在 `%ProgramFiles(x86)%`
- `UninstallDisplayName`：x64 版 `StickyTodo`（无后缀），arm64 版 `StickyTodo (arm64)`（带括号架构标）。仅 `UninstallDisplayName` 分架构；`AppName` / Start Menu `DefaultGroupName` / 桌面快捷方式全都是裸 `StickyTodo`（匹配 Chrome / VS Code / Zoom 的做法）
- `MinVersion=10.0.19041` 卡住 Windows 10 20H1 为最低版本，与 DirectWrite / Direct2D 现代特性匹配；早期 Windows 10 和 Windows 8.1 会在安装时被 Inno Setup 直接拒绝
- `Source:` 段落以 iscc 的 `/DArtifactDir=...` 参数为基准。`package-win-client.sh` 通过 `/DArtifactDir=$(to_win "$OUT_DIR/$ARTIFACT_BASE")` 把已构建好的 `dist/win-client/stickytodo-<ver>-windows-<arch>/` 目录路径传给 iscc（staging 目录名也带架构后缀，两架构并行构建不会互相覆盖），`.iss` 里写 `Source: "{#ArtifactDir}\stickytodo.exe"` 解析即生效——**不**需要先复制到 `installer/` 下做 staging。`.iss` 的 fallback `#define ArtifactDir` 也按 `AppArch` 分支（`build\release` vs `build\release-arm64`）

### 资源编译纪律

- `client/win/src/res/app.rc` 里的 `FILEVERSION` / `PRODUCTVERSION` 必须是 4 段数字（如 `1,0,0,0`），不能写 `dev` / `1.2.3-rc1` 这种语义版本。当前通过 `#define VER_MAJOR/MINOR/PATCH/BUILD` 硬编码为 `1,0,0,0` —— **尚未**与 `$VERSION` 联动，未来做联动时要在 CMakeLists 里解析 `APP_VERSION` 字符串 → 拆 4 段数字 → `configure_file` 模板替换
- 图标资源：`client/win/src/res/app.rc` 引用 `icons\\stickytodo.ico`（相对 `.rc` 自身目录）。该 ico 由 `scripts/generate-icons.sh` 的 `build_windows_ico` 段落从 `assets/branding/stickytodo-icon.svg` 自动派生，包含 **16/20/24/32/40/48/64/128/256** 共 9 档帧（256 走 Vista+ 的 PNG 压缩格式嵌入），覆盖 Taskbar / Alt-Tab / Start Menu / Explorer 各 DPI 所有请求尺寸——**禁止**只 checkin 单档小图。打包器优先级：`magick` → `convert` → `icotool` → `png2ico`（后者不支持 256 帧）；更新品牌时改完 SVG 后跑 `scripts/generate-icons.sh`（或 `--win-only`）即可，ico 仍 checkin 入库（Windows rc 编译链路需要它作为 `ICON` 资源的物理文件）。调用约束：`--mac-only` / `--web-only` / `--win-only` 三者互斥，同时传会 `exit 2`
- Manifest：`client/win/src/res/app.manifest` 声明 DPI 感知、UTF-8 活动代码页、Common Controls v6，通过 `1 RT_MANIFEST "app.manifest"` 嵌入 exe。修改 manifest 不需要改 CMake（rc 引用是相对路径），但修改后必须做一次干净构建（删 `build/release/` 重配），因为 MSBuild / Ninja 有时检测不到 manifest 内容变化
