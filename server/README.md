# TODO Server

单用户 TODO 后端，基于 Gin + GORM + SQLite + JWT。与 `client/` 下的 macOS SwiftUI 便签客户端配套使用。

## 特性

- 单账号登录（用户名 / 密码来自环境变量），JWT（HS256）鉴权，登录恒定时间比较
- Todo CRUD + 完成 / 重开 / 软删 / 恢复；按状态、标签、关键词、截止时间、软删范围过滤
- 业务层审计日志（含变更前后字段 diff 快照）
- SQLite WAL 模式 + 单写连接池；优雅停机 + 强制 DB 关闭
- Docker 多阶段构建，非 root 运行，HEALTHCHECK

## 目录结构

```
server/
├── cmd/todo-server/main.go            # 入口：加载配置→打开 DB→路由→HTTP 优雅停机
├── internal/
│   ├── config/                        # env 解析与校验
│   ├── model/                         # GORM 模型 + Open(AutoMigrate + WAL)
│   ├── repository/                    # 持久层（字段白名单 + 显式 NULL 排序）
│   ├── service/                       # auth / todo / audit 业务
│   ├── middleware/                    # JWT 鉴权中间件
│   ├── handler/                       # HTTP 处理器
│   └── router/                        # 路由与中间件装配
├── scripts/smoke.sh                   # 真机 smoke 脚本（不依赖 jq）
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

- **基础镜像**：`gcr.io/distroless/static-debian12:nonroot`（无 shell、无包管理器、仅 `/app/todo-server` 一个二进制 + 必要 CA 证书）
- **非 root 运行**：uid=65532；宿主机 bind-mount 的 `./data/` 必须对 uid=65532 可写（Docker Desktop for macOS/Windows 通过文件共享层自动转换，Linux 原生需 `chown -R 65532:65532 data`）
- **健康检查**：`HEALTHCHECK` 调用二进制的 `-healthcheck` 子命令（distroless 里没 curl/wget，所以把探针写进了 Go 代码）
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

## 数据目录

- 本地默认 `./data/todo.db`；容器内 `/data/todo.db`
- WAL 会额外生成 `todo.db-wal` / `todo.db-shm`，属正常现象
- **`todo.db` 同时持有业务数据（`todos` / `audit_logs`）与 JWT 签名密钥（`app_secrets` 表，`key='jwt_secret'`）**。删除该文件会让 server 在下次启动时生成新密钥，所有已签发的 token 立刻失效；备份/迁移时务必整库拷贝，不要只导出业务表
- 备份前停服，或至少等待无活跃事务后整目录拷贝

## 版本信息

编译时通过 `-ldflags "-X main.version=<tag>"` 注入；`/health` 返回当前构建版本，便于运维排查。
