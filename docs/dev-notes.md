# 开发注意事项

跨端开发时容易踩的坑、必须遵守的纪律、以及常见开发场景的标准动作。

---

## 验证和回归

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

## 四端字段必须同步

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

---

## React Hooks 规则

`HistoryView` 曾经踩过"把 `useQuery` 放到条件三元里"的坑，导致 query state slot 错位。**所有 hooks 必须无条件调用**，想切不同数据源就在 `queryKey` / `queryFn` 内部用 `if`。

---

## zustand persist 注意事项

**云端数据源重构后，`stickyStore` 已删除**——便签数据由 `/api/sticky-notes` 配合 TanStack Query 管理，无需前端持久化。当前仅 `authStore`（token + username）和 `uiStore`（深色模式）走 `zustand/persist`。

这两个 store 结构都非常简单（字符串 / 枚举），目前**没有 `version` + `migrate` 的需求**。但如果未来给它们加字段，需要遵守：

1. 浏览器端的持久化一定会存在老版本，不要假设 `localStorage` 里的旧数据结构完整
2. 结构性不兼容变更（重命名字段、拆对象、必填字段）必须配合 `version: N` + `migrate: (persisted, fromVersion) => ...` 升级；additive 变更（新增可选字段）也建议在 `onRehydrateStorage` / 自定义 `merge` 里做一次性校验
3. 如果将来 `stickyStore` 回归（例如需要离线缓存），要重新参考老版本 git 历史里 `normalizeSticky` 的兜底模式，而不是在 reducer 里假设字段都齐全

---

## 不要把 WebUI 当静态资源扔出去

`server/internal/webui/dist/` 是 **build 产物镜像目录**，它**不应该**出现在 git 里（除了 `.gitkeep`）。开发时如果想本地跑带 Web 的 server：

```bash
./scripts/package-web.sh      # 先构建 web 并同步（加 ./ 前缀，避免误走 PATH）
cd server && go run ./cmd/todo-server
```

没跑 `package-web.sh` 也能 `go run`，只是 `/app/` 会返回 placeholder 页提示你去构建，不会崩。

---

## 常见开发场景

### 加一个业务字段（例如给 TODO 加 `assignee`）

1. `server/internal/model/models.go` 加字段 + JSON tag（`AutoMigrate` 会自动建列）
2. `server/internal/repository/` **通常无需改动**——`TodoRepo.Update(ctx, id, fields map[string]interface{})` 是动态 `Updates(map)`，新增字段只要 handler 把它放进 map 就行；仅当需要新增按该字段查询/排序的专用方法时才改 repo
3. `server/internal/service/` 如果要做字段级校验就加校验；**审计 diff 无需特殊处理**——`audit_service.go` 把整块变更 struct JSON 化写入 `Detail`，新字段自动被记录
4. `server/internal/handler/` DTO 映射（请求体绑定 + 响应序列化），并把新字段加入 Update handler 构造的 map
5. `client/web/src/types/api.ts` 加字段
6. `client/mac/stickytodo/Models/Todo.swift` 加字段（`Codable`，和 JSON tag 同名即可）
7. `client/win/src/models/Todo.h` 加字段 + `client/win/src/codec/JsonHelper.cpp` 的 `ParseTodo` / `TodoToJson` 两处都要补（**不做**就是数据丢失，不是崩溃——见[四端字段必须同步](#四端字段必须同步)）
8. 跑 `smoke.sh` 确认不破坏现有流程；Windows 端跑 `( cd client/win && cmake --preset debug && cmake --build --preset debug && cd build/debug && ctest --output-on-failure -C Debug )` 确认 `test_models_json` + `test_sticky_codec` 的 JSON 往返测试仍过

### 加一个 API 端点

1. `server/internal/service/` 先写纯业务逻辑
2. `server/internal/handler/` 加 Gin handler
3. `server/internal/router/router.go` 注册路由——鉴权接口挂到 `authed := r.Group("/api")` 下；无需鉴权（如 `/api/login`）直接挂到 `r.` 上
4. `server/scripts/smoke.sh` 里补一步回归
5. 客户端各补一个调用方法：
   - Web：`client/web/src/api/client.ts` 加一个 `api.xxx` 方法 + 必要时 `src/api/queryKeys.ts` 加 cache key + `src/types/api.ts` 加 DTO
   - macOS：`client/mac/stickytodo/Networking/Endpoints.swift` 加 URL 构造器 + `APIClient.swift` 加 `async throws` 方法
   - Windows：`client/win/src/core/HttpClient.h` 加 public 方法声明 + `HttpClient.cpp` 实现（参考现有 `CreateTodo` / `UpdateTodo` / `UpsertSticky` 写法：`WinHttpOpenRequest` → `WinHttpAddRequestHeaders` 加 `Authorization: Bearer` → `WinHttpSendRequest` → `WinHttpReadData` → `JsonHelper` 解析）。**不需要**手写 cache——Windows 端没有 TanStack Query，UI 组件调 `AppState` 的对应方法，`AppState` 调 `HttpClient` 得到结果后通过 `on*Changed` 回调广播给 UI

### 加一个 WS 事件类型（例如 "todo.archived"）

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

### 改 Web UI

- 组件在 `client/web/src/components/`，业务数据用 TanStack Query 拿
- 纯前端状态（便签位置、折叠状态、深色模式）放 Zustand
- 跑 `npm run dev`，Vite 会代理 `/api` 到后端，本地前后端分离联调

### 改 Windows UI

- 控件在 `client/win/src/ui/`，业务数据全都从 `AppState` 拉（`state->GetTodos()` / `state->GetStickies()`），**不要**在 UI 层搞本地缓存
- 所有绘制都走 `D2DRenderer`（Direct2D + DirectWrite），不要引入 GDI / GDI+ 混合绘制——DPI 感知和子像素字体渲染行为会不一致
- 自定义控件（`Button` / `CheckBox` / `TextBox` / `ScrollView` 定义在 `Controls.h/cpp`）用"state 机器 + `Draw(D2DRenderer&)`" 模式。加新状态（如 `disabled` / `selected`）时要同时更新 `HitTest` / `OnMouseMove` / `Draw` 三处
- 调试绘制边界时可以临时在 `Draw` 里 `renderer.DrawRectangle(rect, ColorRGB(1,0,0,0.5), 1.0f)` 画红框，**不要**留到 commit 里
- 本地迭代用 `cmake --preset debug && cmake --build --preset debug`，然后直接 `build/debug/stickytodo.exe` 跑；改完 DTO 或 codec 后跑一次 `( cd build/debug && ctest --output-on-failure -C Debug )` 跑全部 2 个测试（`sticky_codec` + `models_json`）确保往返不破坏

### 发版

见 [RELEASE.md](RELEASE.md)。简化流程：`git tag v1.2.3 && git push --tags`，CI 会自动把 7 份 server 二进制、macOS DMG、Windows portable zip + setup.exe、Docker 镜像都打好并挂到 Release。
