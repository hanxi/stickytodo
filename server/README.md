# TODO Server

单用户 TODO 后端，基于 Gin + GORM + SQLite + JWT。与 `client/` 下的 macOS SwiftUI 便签客户端配套使用。

## 特性

- 单账号登录（用户名 / 密码来自环境变量），JWT（HS256）鉴权，登录恒定时间比较
- Todo CRUD + 完成 / 重开 / 软删 / 恢复；按状态、标签、关键词、截止时间、软删范围过滤
- **Sticky Notes 跨端同步**：`/api/sticky-notes` 提供幂等 upsert / 软删，客户端生成 UUID 字符串主键
- **WebSocket 实时事件推送**（`/api/ws`）：REST 写操作成功后向所有已鉴权连接广播 `todo.*` / `sticky.*` 事件帧；浏览器 / macOS 客户端收到后通过全量 refetch 保持本端 cache 与服务端一致
- 业务层审计日志（含变更前后字段 diff 快照，sticky 变更亦写入 `sticky_upsert` / `sticky_delete` action）
- SQLite WAL 模式 + 单写连接池；优雅停机 + 强制 DB 关闭
- Docker 多阶段构建，非 root 运行

## 目录结构

```
server/
├── cmd/todo-server/main.go            # 入口：加载配置→打开 DB→组装 Deps→路由→HTTP 优雅停机
├── internal/
│   ├── config/                        # env 解析与校验
│   ├── model/                         # GORM 模型（Todo / AuditLog / AppSecret / StickyNote）
│   │                                  # + Open(AutoMigrate + WAL + busy_timeout + FK)
│   ├── repository/                    # 持久层（字段白名单 + 显式 NULL 排序）
│   ├── service/                       # auth / todo / audit / sticky 业务；
│   │                                  # broadcaster.go 定义 EventBroadcaster interface
│   │                                  # （nopBroadcaster 默认实现，ws.HubBroadcaster 生产实现）
│   ├── middleware/                    # JWT 鉴权中间件（仅 auth.go）
│   ├── handler/                       # HTTP 处理器
│   ├── router/                        # 路由与中间件装配（Deps + Build + corsMiddleware）
│   ├── webui/                         # //go:embed all:dist + SPA fallback + CSP/安全头
│   └── ws/                            # WebSocket 实时事件广播：
│                                      # event.go（5 种事件类型常量 + close code 4401/4400）
│                                      # hub.go（广播中枢，不做事件缓冲）
│                                      # client.go（单连接读写 pump、ping/pong）
│                                      # handler.go（/api/ws 的 gin.HandlerFunc + CheckOrigin）
│                                      # adapter.go（HubBroadcaster 实现 service.EventBroadcaster）
├── scripts/
│   ├── smoke.sh                       # 36 步端到端冒烟；不依赖 jq，Step 33-36 跑 WebSocket 回归
│   └── ws-probe/main.go               # WebSocket 回归探针（smoke 启动时 go build 到 mktemp）；
│                                      # 4 种模式：no-auth / bad-token / auth-ready / wait-event
├── Dockerfile
├── docker-compose.yml
├── .env.example
├── go.mod / go.sum
└── README.md
```

## 环境变量

所有变量均在 `internal/config/config.go` 解析，非法值启动时直接报错退出。

| 变量 | 必填 | 默认 | 说明 |
|---|---|---|---|
| `TODO_USERNAME` | 是 | - | 登录用户名（去首尾空格） |
| `TODO_PASSWORD` | 是 | - | 登录密码 |
| `TODO_PORT` | 否 | `8080` | 监听端口，必须为 1-65535 |
| `TODO_DATA_DIR` | 否 | `./data` | SQLite 数据目录（容器内固定 `/data`） |
| `TODO_TOKEN_TTL` | 否 | `24h` | Go duration 格式，必须 > 0 |
| `TODO_VERBOSE` | 否 | `false` | 接受 `1/0/true/false/yes/no/on/off`，非法值报错 |
| `TODO_GIN_MODE` | 否 | `release` | `debug` / `release` / `test` |
| `TODO_CORS_ORIGINS` | 否 | 空 | 逗号分隔精确 origin 列表；留空不注入 CORS；`*` 放开所有 |

> **JWT 签名密钥不走环境变量**。首次启动时 server 会生成 32 字节随机 hex 并写入 SQLite 的 `app_secrets` 表（`key='jwt_secret'`），后续启动从同一张表读回，保证已签发的 token 在 server 重启后仍然有效。若要强制让所有 token 失效，从表里删除该行即可。

## 命令行参数

三个最常用的配置项额外提供了 CLI flag，**flag 优先级高于同名环境变量**；flag 留空则完全回退到环境变量（或默认值）。其余配置项（`TODO_DATA_DIR` / `TODO_TOKEN_TTL` / `TODO_GIN_MODE` / `TODO_VERBOSE` / `TODO_CORS_ORIGINS`）目前仍只能通过环境变量配置。

| Flag | 对应环境变量 | 示例 |
|---|---|---|
| `-port` | `TODO_PORT` | `./todo-server -port 10086` |
| `-username` | `TODO_USERNAME` | `./todo-server -username admin` |
| `-password` | `TODO_PASSWORD` | `./todo-server -password 's3cret'` |
| `-version` | —— | 打印 `-ldflags -X main.version=...` 注入的版本号并退出 |

示例（flag 覆盖已有环境变量）：

```bash
TODO_PORT=8080 ./todo-server -port 10086 -username admin -password test123
# 进程最终监听 :10086，账号 admin / test123
```

## 本地运行

要求：Go 1.25+，CGO 工具链。macOS 执行 `xcode-select --install` 安装 Command Line Tools；Linux 安装 `build-essential`。

```bash
cd server
go mod download
go build ./... && go vet ./...

TODO_USERNAME=admin \
TODO_PASSWORD=test123 \
TODO_DATA_DIR=./data \
go run ./cmd/todo-server
```

默认监听 `http://127.0.0.1:8080`。

### 真机 smoke

保持 server 在跑，另开终端：

```bash
cd server
BASE_URL=http://127.0.0.1:8080 \
TODO_USERNAME=admin TODO_PASSWORD=test123 \
bash scripts/smoke.sh
```

所有步骤打印 `[PASS]` 即通过；任一步失败脚本立即退出 1。

## Web UI（`/app/`）

前端源码在 `client/web/`（React + Vite + Tailwind），通过 `go:embed` 编译进 server 二进制。路由挂载规则：

| 路径 | 行为 |
|---|---|
| `/app` | `301` 永久重定向到 `/app/` |
| `/app/` | 返回 `index.html`（React SPA 入口） |
| `/app/assets/*` | 静态资源（`Cache-Control: public, max-age=31536000, immutable`） |
| `/app/<不存在的前端路径>` | SPA fallback，仍返回 `index.html`，由前端路由接管 |

HTTP 响应头带 `Content-Security-Policy: default-src 'self'; style-src 'self' 'unsafe-inline'; ...`，默认仅允许同源请求。

发布前需要先在仓库根跑 `bash scripts/package-web.sh`，产物会同步到 `server/internal/webui/dist/`——这个目录在 `.gitignore` 里（只保留 `.gitkeep`），本地不跑 web build 的话 `go run` 可能 embed 空目录。

## Docker 运行

```bash
cd server
cp .env.example .env
# 至少填 TODO_USERNAME / TODO_PASSWORD；JWT 密钥由 server 自动生成到 ./data/todo.db
docker compose build
docker compose up -d
docker compose logs -f
```

数据持久化到宿主机 `./data/`。停止：`docker compose down`（不删除 `./data/`）。

### 镜像特性

- **基础镜像**：`alpine:3.20`（自带 shell + apk，安装了 `ca-certificates` 与 `tzdata`）
- **root 运行**：容器进程默认以 root 启动，宿主机 bind-mount 的 `./data/` 不需要任何 `chown` 预处理即可写入；威胁模型按"自托管单租户、宿主只跑自己的代码"假设
- **架构覆盖**：`linux/amd64`、`linux/arm64`、`linux/arm/v7`（由 `docker/build-push-action` 在 GitHub Actions 里跨架构构建）

官方镜像：`docker.io/hanxi/stickytodo:<tag>` 与 `docker.io/hanxi/stickytodo:latest`。

## API 速查

所有 `/api/*`（除 `/api/login`）需携带 `Authorization: Bearer <token>`。

| 方法 | 路径 | 说明 |
|---|---|---|
| `GET` | `/health` | `{status,time,server,version}` |
| `POST` | `/api/login` | 入参 `{username,password}` → 出参 `{token,expires_at,username}` |
| `GET` | `/api/todos` | 分页列表，见下方查询参数 |
| `POST` | `/api/todos` | 新建 `{title,content?,priority?,tag?,due_at?}` |
| `GET` | `/api/todos/:id` | 详情；支持 `?include_deleted=1` |
| `PUT` | `/api/todos/:id` | 更新；任何字段可选，至少传一个；`clear_due_at=true` 清空截止时间 |
| `DELETE` | `/api/todos/:id` | 软删；返回 `{id,deleted:true}` |
| `POST` | `/api/todos/:id/complete` | 标记完成（幂等） |
| `POST` | `/api/todos/:id/reopen` | 重新打开（幂等） |
| `POST` | `/api/todos/:id/restore` | 从软删恢复 |
| `GET` | `/api/todos/:id/history` | 该 TODO 的审计分页；`page`, `page_size` |
| `GET` | `/api/audit-logs` | 全局审计；`action`, `actor`, `todo_id`, `from`, `to`(RFC3339), `page`, `page_size` |
| `GET` | `/api/tags` | `{tags:[...]}` 去重有序标签 |
| `GET` | `/api/sticky-notes` | `{items:[...]}` 所有便签，按 `updated_at DESC` 排序 |
| `GET` | `/api/sticky-notes/:id` | 单条便签 |
| `PUT` | `/api/sticky-notes/:id` | 幂等 upsert；`id` 为客户端生成的 UUID 字符串 |
| `DELETE` | `/api/sticky-notes/:id` | 软删；返回 `{id,deleted:true}` |
| `GET` | `/api/ws` | WebSocket 升级；握手后走首帧 auth 协议，**不**带 `Authorization` header（浏览器 `WebSocket` 构造器不支持自定义 header，见下节） |

### `GET /api/ws` WebSocket 实时事件通道

与 REST 接口不同，`/api/ws` 的鉴权不经过 `middleware.Auth`（也不接受 `Authorization: Bearer` 头），而是走"首帧 auth"协议：

1. **HTTP → WebSocket 升级**：客户端发 `GET /api/ws` + 标准 `Upgrade: websocket` 头；服务端 `CheckOrigin` 按 `TODO_CORS_ORIGINS` 精确匹配，此外无条件放行"缺 Origin 头的非浏览器客户端"以及"同源握手"
2. **首帧 auth**（C → S，必须在 2 秒内）：
   ```json
   { "type": "auth", "token": "<jwt>" }
   ```
3. **ready 回执**（S → C）：auth 成功后服务端回：
   ```json
   { "type": "ready", "server_time": "2024-01-01T00:00:00Z" }
   ```
   客户端收到 ready 才算握手完成，之后开始收业务事件。
4. **业务事件帧**（S → C）：每次 REST 写操作成功后 hub 向所有已鉴权客户端广播。事件类型共 5 种：

   | type | 载荷 | 触发接口 |
   |---|---|---|
   | `todo.created` | `{"type":"todo.created","data":<完整 Todo JSON>}` | `POST /api/todos` |
   | `todo.updated` | `{"type":"todo.updated","data":<完整 Todo JSON>}` | `PUT /api/todos/:id`、`/complete`、`/reopen`、`/restore` |
   | `todo.deleted` | `{"type":"todo.deleted","id":<uint>}` | `DELETE /api/todos/:id` |
   | `sticky.upserted` | `{"type":"sticky.upserted","data":<完整 StickyNote JSON>}` | `PUT /api/sticky-notes/:id` |
   | `sticky.deleted` | `{"type":"sticky.deleted","id":"<string>"}` | `DELETE /api/sticky-notes/:id` |

   资源变更类事件用 `data` 承载完整资源 JSON；删除类事件只回 `id`，避免把被删资源重传一次。**注意**：hub 不做 sender 过滤——发起写请求的客户端本身也会收到同一事件，客户端必须能区分"本端 mutation 刚完成"与"收到 WS 广播"两条路径、避免重复操作。

5. **Close code**（服务端主动关闭连接时发送）：

   | code | 语义 | 客户端实际行为（`ws.ts` / `RealtimeClient.swift`） |
   |---|---|---|
   | `4401` | auth 超时（未在 2s 内发 auth 帧）/ token 非法 / token 过期 | **不重连**，emit `unauthorized` 信号，上层据此清 token 走登出流程 |
   | `4400` | 客户端发了非法上行业务帧（除首帧 auth 外，`/api/ws` 不接受任何上行业务消息） | **走默认指数退避重连**——两端实现均只在 close code 为 4401 时走"不重连"分支，其他所有 close code（含 4400、1000、1006 等）都进入 `scheduleReconnect` |

6. **心跳**：服务端每 30s 发 WebSocket ping；60s 内没收到 pong（`pongWait`）连接会被判定为死连接并移除。浏览器 `WebSocket` API 会自动响应服务端 ping，无需应用处理。macOS 客户端在此之外还跑了一个 15s 周期的本端 `sendPing`——目的是**尽早探测本端到服务端链路是否已断**（弱网或 NAT 超时场景下，`URLSessionWebSocketTask.receive()` 可能长时间既不返回帧也不抛错），本端 ping 一旦发送失败就能从 completion / 下一次 receive 尽快报错并触发重连。

   > ⚠️ 注意：服务端 `client.go` 只注册了 `SetPongHandler`、**没有** `SetPingHandler`——gorilla/websocket 默认 ping handler 只自动回 pong、**不会重置读超时**。所以延续读超时的**唯一**路径是"服务端 ping → 客户端 pong"；客户端单向发 ping 不会帮服务端保活，只能帮客户端自己尽早察觉死连接。

7. **重连与事件补偿**：hub **不缓冲**历史事件（`hub.go:Broadcast` 只做非阻塞扇出，没有 ring buffer / replay 机制）——断线期间发生的写操作不会在重连后回放。客户端必须在重连成功后全量 refetch 自己关心的资源（Web 端 `useRealtimeSync` 监听 `reconnected` signal 触发 `queryClient.invalidateQueries()`；macOS 端 `AppState` 通过 `NotificationCenter` 扇出 `.stickyRealtimeReconnected` 给所有 `StickyViewModel` 去抖刷新）。两端内置的退避时间表均为 `[1, 2, 4, 8, 16, 30]` 秒；走到末尾恒定使用最后一档。

真机回归见 `scripts/smoke.sh` 的 Step 33-36，它在脚本开头先 `go build ./scripts/ws-probe` 产出一个临时二进制，再在步骤里以 4 种模式调用：
- `no-auth`（Step 33）：连上后故意不发 auth，断言服务端在 `authTimeout` 内以 4401 关闭
- `bad-token`（Step 34）：发送 token=`BAD` 的 auth 帧，断言服务端以 4401 关闭
- `auth-ready`（Step 35）：用登录成功拿到的合法 token auth，断言收到 `{"type":"ready"}`
- `wait-event`（Step 36）：auth + ready 后后台等事件，主脚本 POST 一个 REST 写操作，断言 WS 在超时前推送出对应 `todo.*` / `sticky.*` 事件

### `GET /api/todos` 查询参数

| 参数 | 说明 |
|---|---|
| `status` | `pending` / `done`；空不过滤 |
| `tag` | 精确匹配 |
| `keyword` | 在 `title` 与 `content` 上做大小写不敏感 LIKE |
| `due_before` | RFC3339 时间，只返回早于该时间的 |
| `include_deleted` | `1/0/true/false`，包含软删记录 |
| `only_deleted` | 仅软删记录（优先级高于 `include_deleted`） |
| `page` | 从 1 开始 |
| `page_size` | 默认 20，最大 200 |

响应结构：`{items:[...], total, page, page_size}`。排序：`priority DESC → due_at 升序（NULL 置后）→ id DESC`。

### 字段与枚举

- `priority`：整数 `0~3`，0 最低
- `tag`：单值字符串（不是数组），长度 ≤ 64
- `title`：必填，长度 ≤ 500
- `content`：长度 ≤ 65536 字节
- `status`：`pending` / `done`

### Sticky Notes 字段说明

便签（`/api/sticky-notes`）用于 macOS / Web 客户端跨端共享浮窗布局。设计上与 Todo 有两点关键差异：

- **主键 `id` 是字符串（客户端生成的 UUID）**，不是自增整数。服务端不重新分配 id；客户端可本地乐观创建后再 `PUT` 幂等落盘。字符集 `[A-Za-z0-9_-]`，长度 1..64。
- **`frame` / `bg_color` / `filter` 是 JSON 字符串字段（不是嵌套对象）**，服务端**不解析内部结构**，只校验：
  - 必须是合法 JSON（`json.Valid`）
  - 序列化后长度 ≤ 4096 字节
  - 空字符串会被规范化为 `"{}"` 再落库

  这样前端可以自由扩展布局/样式/筛选字段，后端 schema 不需要跟随迭代。典型内容：
  - `frame`：`{"x":100,"y":100,"width":300,"height":420}`
  - `bg_color`：`{"red":1,"green":0.92,"blue":0.54,"alpha":1}`
  - `filter`：`{"status":"pending","tag":"work","page":1,"page_size":50}`（具体字段由客户端自定义）

- **`title` 长度 ≤ 200**，会在写入前 `TrimSpace`。
- **排序**：列表按 `updated_at DESC` 返回；同一毫秒内按 `id DESC` 稳定排序。不返回分页游标（预期便签总量 < 50，一次拉全）。
- **无跨便签顺序字段**：多便签网格/多窗口的视觉位置由 `frame.x/y` 决定，服务端不维护显式 `z_index` 或 `rank`。
- **审计**：upsert/delete 会分别写入 `sticky_upsert` / `sticky_delete` action 到 `audit_logs`，`todo_id` 字段为 NULL（便签不复用 todo_id 索引语义）。

### 错误响应

统一 `{"error":"<message>"}`。

| HTTP | 场景 |
|---|---|
| `400` | 请求体 / 参数不合法（JSON、枚举、长度、无更新字段） |
| `401` | 未登录、token 失效 / 过期、用户名密码错误 |
| `404` | 资源不存在或未包含软删 |
| `405` | HTTP 方法不允许 |
| `500` | 服务端异常 |

`/api/ws` 握手失败属于 HTTP 层错误（`upgrader.Upgrade` 自行写 `400 Bad Request`），不走 JSON 错误体；握手后的鉴权/协议违规通过 WebSocket Close 帧 + application-level close code（`4401` / `4400`）表达，见上方「`GET /api/ws`」节。

## 数据目录

- 本地默认 `./data/todo.db`；容器内 `/data/todo.db`
- WAL 会额外生成 `todo.db-wal` / `todo.db-shm`，属正常现象
- **`todo.db` 同时持有业务数据（`todos` / `audit_logs`）与 JWT 签名密钥（`app_secrets` 表，`key='jwt_secret'`）**。删除该文件会让 server 在下次启动时生成新密钥，所有已签发的 token 立刻失效；备份/迁移时务必整库拷贝，不要只导出业务表
- 备份前停服，或至少等待无活跃事务后整目录拷贝

## 版本信息

编译时通过 `-ldflags "-X main.version=<tag>"` 注入；`/health` 返回当前构建版本，便于运维排查。
