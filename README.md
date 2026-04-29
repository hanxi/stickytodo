# stickytodo

一个自托管的多便签 TODO 工具：单账号 JWT 鉴权、浏览器即开即用、macOS 原生菜单栏客户端。

- GitHub：`github.com/hanxi/stickytodo`
- Docker：`docker.io/hanxi/stickytodo`

> 想了解项目架构、模块边界、开发约定？请看 [AGENTS.md](./AGENTS.md)。

## 功能一览

- 单账号登录（用户名 / 密码通过环境变量配置）
- TODO 增删改 / 完成 / 软删除 / 恢复
- 单条 TODO 变更历史 + 全局操作审计日志
- 多便签：Web 网格布局 / macOS 每便签一窗，支持独立筛选、颜色、置顶
- **跨端实时同步**（WebSocket `/api/ws`）：在任一端（浏览器 / macOS 客户端）新建、修改、删除一条 TODO 或便签，其他已登录在线的客户端会在 ~300ms 去抖窗口后自动刷新（客户端侧去抖窗口 + 服务端 hub 即时广播），无需手动按刷新；网络断连后自动指数退避重连并全量拉取
- 便签本身**跨端同步**（服务端是唯一数据源）；macOS 端的便签**窗口位置**仅保存在本机 UserDefaults，不跨端同步（浏览器"便签"本质是 Card 也不需要位置）
- 后端单二进制部署，Web UI 通过 `go:embed` 内嵌，零额外静态资源

## 前置依赖

只需按你要用的形态装对应依赖即可，**全部都是可选组合**。

| 场景 | 依赖 |
|---|---|
| 用 Docker 部署后端 + 浏览器访问 Web UI | Docker 20.10+ 且内置 Compose V2（`docker compose` 子命令，非旧版 `docker-compose` 脚本）|
| 本地源码跑后端 | Go 1.25+（与 `server/go.mod` 对齐） |
| 从源码构建 macOS 客户端 | macOS 13+、**完整 Xcode 15+**（非 Command Line Tools；仓库在 Xcode 26.4 下验证） |
| 本地开发 Web 客户端 | Node.js **18 / 20 / 22+** 之一、npm（Vite 5 官方只支持 `^18 \|\| ^20 \|\| >=22`，奇数 major 不受支持；`client/web/package.json` 未强制 `engines` 字段，自行遵守即可）|

> 💡 **JWT 密钥免配置**：server 首次启动时生成 32 字节随机熵、hex 编码后（DB 里 64 字符）持久化到 SQLite `app_secrets` 表的 `key='jwt_secret'` 行，重启复用；想强制失效所有 token，删掉该行重启即可。

## 安装和运行

### 方式 A：Docker（推荐）

```bash
docker run -d --name stickytodo -p 8080:8080 \
  -e TODO_USERNAME=admin \
  -e TODO_PASSWORD=change-me-please \
  -v $(pwd)/data:/data \
  docker.io/hanxi/stickytodo:latest
```

或使用仓库里的 compose 文件：

```bash
cd server
cp .env.example .env          # 至少改 TODO_USERNAME / TODO_PASSWORD
docker compose up --build -d  # 数据持久化到 server/data/（compose 里是 ./data:/data，
                              # 以 compose 文件所在目录为基准）
```

> 数据落盘位置取决于**你在哪个目录跑命令**：`docker run -v $(pwd)/data:/data ...` 存到当前目录下的 `data/`；`docker compose` 存到 `server/data/`（compose 文件自己在 `server/` 下）。别混着用，否则换方式启动后会看似"数据丢了"。

官方镜像覆盖 `linux/amd64`、`linux/arm64`、`linux/arm/v7`，由 GitHub Actions 在打 tag 时推送。

### 方式 B：源码直接跑后端

```bash
cd server
export TODO_USERNAME=admin TODO_PASSWORD=change-me-please  # 值与 .env.example 保持一致，便于跑 smoke.sh
go run ./cmd/todo-server
# 默认监听 :8080（0.0.0.0:8080），本机访问 http://127.0.0.1:8080/
# 数据存于 ./data/todo.db（config.go 默认 TODO_DATA_DIR=./data）
```

> ⚠️ 仓库里的 `server/.env.example` 是为 **Docker Compose** 准备的，里面 `TODO_DATA_DIR=/data` 指的是**容器内**路径。如果你本地直接 `go run`，不要 `source` 它；需要覆盖数据目录时用 `export TODO_DATA_DIR=./data`（或任意本地绝对路径）。

### 方式 C：预编译二进制

从 [Releases](https://github.com/hanxi/stickytodo/releases) 下载对应平台的二进制（覆盖 linux / darwin / windows × amd64/arm64，以及 linux armv7），**先用同目录的 `SHA256SUMS` 校验完整性**：

```bash
# 1) 校验下载完整性（和二进制放同一目录，任选其一即可）
sha256sum  -c SHA256SUMS    # Linux
shasum -a 256 -c SHA256SUMS # macOS（系统自带 shasum）

# 2) macOS / Linux：加执行位
chmod +x stickytodo-server-<version>-<os>-<arch>

# 3) macOS 补充：浏览器下载的产物会带 com.apple.quarantine 扩展属性，首次会被 Gatekeeper 拦
xattr -dr com.apple.quarantine stickytodo-server-<version>-darwin-<arch>

# 4) 运行（数据默认落到当前工作目录的 ./data/，与 Docker 镜像默认 /data 不同）
export TODO_USERNAME=admin TODO_PASSWORD=change-me-please
./stickytodo-server-<version>-<os>-<arch>
```

Windows 直接双击 `.exe` 即可；`chmod +x` 和 `xattr` 两步仅 Unix 平台需要。

## 访问 Web UI

浏览器打开 `http://127.0.0.1:8080/app/`（结尾斜杠不能少，裸 `/app` 会 301 重定向到 `/app/`）。用**当前运行的 server 进程所使用的** `TODO_USERNAME` / `TODO_PASSWORD` 登录——`docker run -e` / `docker compose` 走 `.env` / `go run` 走 `export`，三种起法账号来源不同，不要搞混。

## 运行 macOS 客户端

**A. 直接下载 DMG**（Releases 里的 `stickytodo-<version>-macos-universal.dmg`）

双击 DMG → 把 `stickytodo.app` 拖到 `/Applications`。首次运行会被 Gatekeeper 警告（ad-hoc 签名），右键 App → 打开，确认一次即可。

**B. 从源码运行**

```bash
# 方法 1：Xcode 打开 client/mac/stickytodo.xcodeproj 后 ⌘R
# 方法 2：命令行一键构建
./client/scripts/build.sh
open /tmp/stickytodoBuild/Build/Products/Debug/stickytodo.app
```

**首次使用**：应用以菜单栏图标 `note.text` 常驻（无 Dock 图标）。点图标 → 「打开设置」（⌘,）进入 Settings 窗口。**Settings 是 3 Tab 的 macOS Preferences 风格面板**：

- 「设置」Tab：填服务端地址（如 `http://127.0.0.1:8080`，可选点「测试连接」验证 → `GET /health`）→ 登录
- 「历史」Tab：登录后查看**全局**审计日志（菜单栏面板里已不再有「历史」按钮，全局入口只在此处）。单条 TODO 的变更历史仍可通过便签窗口内 TODO 行末尾的 `⋯` 菜单 →「历史」以 sheet 形式打开，作用域仅该条
- 「关于」Tab：版本号 / Bundle ID / 项目链接

登录后回到菜单栏面板点「新建便签」（⌘N）即可。更多操作 / 快捷键见 [client/mac/README.md](./client/mac/README.md)。

## 配置项

全部通过环境变量传入，与后端 `.env.example` 一致：

| 变量 | 必填 | 默认 | 作用范围 | 说明 |
|---|---|---|---|---|
| `TODO_USERNAME` | ✅ | — | server | 登录用户名 |
| `TODO_PASSWORD` | ✅ | — | server | 登录密码 |
| `TODO_PORT` | ❌ | `8080` | server 进程 | server 进程监听端口（无论跑在裸机、容器还是 docker compose 里，都是进程本身绑定这个端口）|
| `TODO_HOST_PORT` | ❌ | `8080` | docker-compose 宿主机 | 宿主机端口 → 容器内 `TODO_PORT` 的映射；**仅** `server/docker-compose.yml` 里会用到，裸 `docker run` 或源码跑都无视此变量 |
| `TODO_DATA_DIR` | ❌ | `./data`（源码）/ `/data`（Docker 镜像） | server | SQLite 存储目录 |
| `TODO_TOKEN_TTL` | ❌ | `24h` | server | JWT 有效期（Go `time.Duration` 格式） |
| `TODO_CORS_ORIGINS` | ❌ | 空（不注入 CORS 中间件） | server | 允许的 Origin allowlist，逗号分隔（精确匹配）；特殊值 `*` 代表放行任意源（见 `.env.example` 注释） |
| `TODO_GIN_MODE` | ❌ | `release` | server | `debug` / `release` / `test` |
| `TODO_VERBOSE` | ❌ | `false` | server | 打开更详细的请求日志（GORM info 级别）。合法取值：`1/true/yes/on/t/y`（true 集）或 `0/false/no/off/f/n`（false 集），大小写不敏感；非法值**启动时直接报错退出**（`config.go#parseBoolEnv`） |

## 文档索引

- [AGENTS.md](./AGENTS.md)：项目架构、模块边界、开发约定、构建发布链路
- [server/README.md](./server/README.md)：后端 API 清单、配置、测试
- [client/mac/README.md](./client/mac/README.md)：macOS 客户端架构、快捷键
- [client/web/README.md](./client/web/README.md)：Web 客户端架构、本地开发、embed 约定
- [docs/RELEASE.md](./docs/RELEASE.md)：本地打包命令、CI 发布流程、所需 secrets

## 💖 支持项目

如果这个项目对你有帮助，欢迎通过以下方式支持：

### ⭐ Star 项目
点击右上角的 ⭐ Star 按钮，让更多人发现这个项目。

### 💰 赞赏支持
- [💝 爱发电](https://afdian.com/a/imhanxi) - 持续支持项目发展
- 扫码请作者喝杯奶茶 ☕

<p align="center">
  <img src="https://i.v2ex.co/7Q03axO5l.png" alt="赞赏码" width="300">
</p>

## License

MIT
