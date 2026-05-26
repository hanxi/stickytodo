# 构建与发布

本节覆盖：本地打包脚本、GitHub Actions 三 workflow、产物矩阵、交叉编译纪律、版本号来源、macOS Xcode/SDK 版本一致性策略。

---

## 本地脚本（`scripts/`）

所有脚本都**设计为可独立跑**，CI 对前 4 个脚本直接复用（`package-docker.sh` 是**本地开发专用**，CI 的 Docker 构建另走 `docker/build-push-action@v6` + buildx，不调用本脚本）：

| 脚本 | 产出 | 依赖 | 读 `VERSION` | CI 复用 |
|---|---|---|---|---|
| `package-web.sh` | `client/web/dist/` + 同步到 `server/internal/webui/dist/` | Node.js、npm | ❌ 不读，固定构建静态产物 | ✅ `build-web` job |
| `package-server.sh` | `dist/server/stickytodo-server-<ver>-<os>-<arch>[.exe]` × 7 + 汇总 `SHA256SUMS` | Go、跑过 `package-web.sh` | ✅ 默认 `dev`，通过 `-ldflags -X main.version=` 注入到 `/health` | ✅ `build-server` job |
| `package-mac-client.sh` | `dist/mac-client/stickytodo-<ver>-macos-universal.dmg`（**或** `--skip-dmg` 时 fallback 成 `stickytodo-<ver>-macos-universal.app.zip`）+ 汇总 `SHA256SUMS`；**注意**发布文件名带版本，但 **DMG / zip 内部的 `.app` bundle 恒为 `stickytodo.app`**（不带版本），这是用户拖到 `/Applications` 后在 Launchpad / Dock 里看到的名字，必须是干净品牌名。DMG 卷标（双击 DMG 后 Finder 窗口标题）为 `stickytodo <ver>` | Xcode **26.4.x**（完整 IDE）；CI 用 `runs-on: macos-26`（**不是** `macos-latest`——后者 YAML label 目前指向 macos-15-arm64，上面根本没装 Xcode 26.x）+ `maxim-lobanov/setup-xcode@v1` **显式锁 Xcode 到 `26.4.1`**（与本机 26.4 同 `macosx26.4` SDK）。详见 [macOS Xcode / SDK 版本一致性](#macos-客户端-xcode--sdk-版本一致性)；DMG 打包优先 `create-dmg`（`brew install create-dmg`），缺失时 fallback 到系统自带 `hdiutil` | ✅ 默认 `dev`，**仅用于发布文件名 + DMG 卷标**，不改 App 内的 `CFBundleShortVersionString`，也不改 `.app` bundle 文件名 | ✅ `build-mac-dmg` job |
| `package-win-client.sh` | `dist/win-client/stickytodo-<ver>-windows-<arch>.zip`（portable 便携包，`<arch>` ∈ `{x64, arm64}`）+ **可选** `stickytodo-setup-<ver>-<arch>.exe`（Inno Setup 安装器；脚本检测不到 `iscc.exe` 或传 `--skip-installer` 时跳过，**不致命**）+ 每架构一份 `SHA256SUMS-<arch>`。portable zip 内顶层是 `stickytodo-<ver>-windows-<arch>/` 文件夹，内含 `stickytodo.exe`（MSVC 默认 dynamic CRT，无需额外打包 VC++ 运行时 — windows-2022 runner 和 Windows 10 19041+ 用户系统自带 VCRUNTIME140.dll；arm64 版的 arm64 CRT 同理随系统）+ 可选 `README.md` + 可选 `LICENSE.txt`（从仓库根 `README.md` / `LICENSE` 复制，缺则忽略）。setup.exe 是 per-user 安装器（默认 `{autopf}\StickyTodo`，per-user 模式下解析为 `%LocalAppData%\Programs\StickyTodo`；可选桌面快捷方式、默认不勾）。与 mac 同理，**文件名带 `<ver>` 和 `<arch>`，但 zip 内部可执行文件恒为 `stickytodo.exe`**（干净品牌名）。**架构靠 `ARCH` 环境变量选择**（默认 `x64`）：`ARCH=x64` 走 `release` preset + `x64-windows` triplet + `ilammy/msvc-dev-cmd arch=amd64`，`ARCH=arm64` 走 `release-arm64` preset + `arm64-windows` triplet + `ilammy/msvc-dev-cmd arch=amd64_arm64`（x64 host → arm64 target 交叉编译）。**arm64 自动跳过 ctest**（x64 runner 跑不了 arm64 二进制，codec/models 纯 C++ 逻辑与架构无关，x64 leg 的测试结果已覆盖） | Windows（CI: `windows-2022`）、MSVC 构建工具链（CI 镜像自带，arm64 需 `amd64_arm64` 交叉工具集，镜像自带）、vcpkg（CI 通过 `VCPKG_ROOT` 环境变量发现，镜像已预装；`arm64-windows` triplet 会触发 cppwinrt / nlohmann-json 的 arm64 重新构建，首次约 3-4 min）、可选 Inno Setup 6（CI 通过 `choco install innosetup` 按需安装）。脚本通过 `cmake --preset release` / `release-arm64` 配置，**不**依赖 `package-web.sh` | ✅ 默认 `dev`，**只用于产物文件名**；当前 `app.rc` 里的 `FILEVERSION` / `PRODUCTVERSION` **硬编码为 `1,0,0,0`**，`StringFileInfo` 里的 `FileVersion` / `ProductVersion` 字符串也硬编码为 `"1.0.0.0"`（见 `client/win/src/res/app.rc` 的 `#define VER_MAJOR/MINOR/PATCH/BUILD` 块）。如果未来要把 `$VERSION` 真正写进 PE 资源，需要改 `app.rc` 为 CMake `configure_file` 模板 + 在 `CMakeLists.txt` 解析 `APP_VERSION` 字符串拆成 4 段数字。当前未实现——与 mac 客户端 `MARKETING_VERSION=1.0` 的现状（见[版本号来源](#版本号来源)）一致 | ✅ `build-win-client` job（`strategy.matrix.arch: [x64, arm64]` 并行两份） |
| `package-docker.sh` | 本地 Docker 镜像（当前平台单架构，不跨平台，`docker build` 而非 `docker buildx build`）| Docker daemon | ✅ 默认 `dev`，也作为镜像 tag | ❌ **CI 不调用**，CI 用 buildx 直推多架构 manifest |

脚本之间的依赖关系：

- 仅 `package-server.sh` 和 `package-docker.sh` 依赖 `package-web.sh` 的产物——它们把 server 编进二进制 / 镜像时会把 `server/internal/webui/dist/` 一并 `go:embed` 进去
- `package-mac-client.sh` **不依赖** web（macOS 是原生 Swift 客户端，不嵌 Web UI）
- `package-win-client.sh` **不依赖** web 同理（Windows 是原生 Win32 + Direct2D 客户端，不嵌 Web UI）。它有自己的依赖链：CMake 配置阶段 vcpkg 会拉取 `nlohmann-json`（运行期，header-only JSON 库）+ `cppwinrt`（**当前代码未使用**——保留为将来接入 Windows Toast Notifications 等 WinRT API 的预留，不占 exe 体积）以及 `gtest`（仅 `debug` preset 通过 `VCPKG_MANIFEST_FEATURES=tests` 激活 `vcpkg.json` 的 `features.tests.dependencies`，release 构建不会拉 gtest）；构建阶段需要的 Windows SDK 库在 `CMakeLists.txt` 里以 `target_link_libraries` 形式声明，当前列表：`d2d1` / `dwrite` / `dxgi` / `windowscodecs` / `credui` / `advapi32` / `shell32` / `ws2_32` / `ole32` / `uuid` / `windowsapp`。`winhttp.lib` **没有**走 CMake `target_link_libraries`，而是在 `WebSocketClient.cpp` 和 `HttpClient.cpp` 两个文件**各自**用 `#pragma comment(lib, "winhttp.lib")` 就地声明——增删系统库时要两处一起 grep（`target_link_libraries` + `grep -rn "#pragma comment(lib" client/win/src`）
- `go build` 本身在 dist 缺失时也能通过（`webui.go` 会回退到内置的 placeholder 页）

另外 `package-web.sh` 的 npm 安装策略是：检测到 `package-lock.json` 走 `npm ci`（可复现）；没有 lockfile 才降级为 `npm install`。所以手动在 `client/web/` 下跑 `npm install` 是开发便利写法，CI / 打包脚本走的是 `npm ci`。

---

## GitHub Actions

三个 workflow 文件分工：

### `_build-all.yml`（`on: workflow_call`）

reusable workflow，输入 `version` / `tag_name` / `prerelease` / `docker_image` / `tag_latest`，包含 7 个 job：

1. **`build-web`**
2. **`build-server`**（矩阵：linux × amd64/arm64/armv7、darwin × amd64/arm64、windows × amd64/arm64）
3. **`build-mac-dmg`**（`runs-on: macos-26` runner——**明确不是** `macos-latest`，因为 runner-images 主 README 明载 `macos-latest` YAML label 当前指向 `macos-15-arm64`（macOS 15.7.x + Xcode 16.x 系列），那上面根本没有 macOS 26 SDK，和本机 Xcode 26.4 无法对齐；而 `macos-26` 是 macOS 26 Tahoe arm64 runner，`xcode-select -p` 默认指 `Xcode_26.2.app`、但预装列表里还有 26.5 beta / 26.4.1 / 26.3 / 26.1.1 / 26.0.1。`+ maxim-lobanov/setup-xcode@v1 xcode-version: '26.4.1'` 显式切到 `macosx26.4` SDK + `brew install create-dmg || true` + `package-mac-client.sh`）。**为什么必须锁 Xcode 而非依赖 macos-26 runner 的默认 26.2**：macOS 26.2 SDK 和本机的 macOS 26.4 SDK 在 SwiftUI `.buttonStyle(.bordered)` 的默认填充基色上不等价——便签加号按钮在 macosx26.2 SDK 下编译出来是**白底**、在 macosx26.4 SDK 下是**灰底**。源码层在 `StickyView.swift` 里加的 `.tint(.secondary)` + `.controlSize(.small)` 是第二道防线（当未来锁定版本因镜像轮转失效时兜底），CI 侧锁 SDK 才是根本解。详见[macOS Xcode / SDK 版本一致性](#macos-客户端-xcode--sdk-版本一致性)
4. **`build-win-client`**（`runs-on: windows-2022` runner——**明确不是** `windows-latest`，保持 runner 镜像固定，避免 MSBuild / Windows SDK 版本随 `-latest` 静默漂移导致产物行为差异。镜像自带 MSVC 2022、CMake ≥ 3.25、Ninja、vcpkg（预装路径由镜像暴露的 `VCPKG_INSTALLATION_ROOT` 提供）。**该 job 启用 `strategy.matrix.arch: [x64, arm64]` 并行两份**（`fail-fast: false` 保证两个 leg 都跑完，maintainers 能同时看到 x64 / arm64 的失败），每个 leg 的 steps 序列：
   1. `actions/checkout@v4`
   2. `Activate MSVC dev environment (${{ matrix.arch }})` —— `ilammy/msvc-dev-cmd@v1`，`arch` 参数用表达式 `matrix.arch == 'arm64' && 'amd64_arm64' || 'amd64'`：x64 leg 走 `amd64`（native x64-host x64-target），arm64 leg 走 `amd64_arm64`（x64-host + arm64-target 交叉编译，cl.exe 仍在 x64 runner 上运行、只是换了 arm64 codegen）
   3. `Install Inno Setup (if missing)` —— pwsh 里先 `Test-Path 'C:\Program Files (x86)\Inno Setup 6\ISCC.exe'`，不存在才 `choco install innosetup --no-progress -y`（幂等，windows-2022 镜像通常预装；`choco install` 的"已安装则 no-op"保证无副作用）
   4. `Export VCPKG_ROOT` —— bash step 把 `VCPKG_INSTALLATION_ROOT` 映射到 `VCPKG_ROOT` 写进 `GITHUB_ENV`，后续 steps 和 `CMakePresets.json` 的 `$env{VCPKG_ROOT}` 才能解析
   5. `Package Windows client (${{ matrix.arch }})` —— `VERSION: ${{ inputs.version }}` + `ARCH: ${{ matrix.arch }}` + `bash scripts/package-win-client.sh`（脚本按 ARCH 选 CMake preset、vcpkg triplet、iscc `/DAppArch` define、产物命名）
   6. `Upload Windows artifacts (${{ matrix.arch }})` —— `actions/upload-artifact@v4` 上传 `dist/win-client/stickytodo-*-windows-${{ matrix.arch }}.zip` + `dist/win-client/stickytodo-setup-*-${{ matrix.arch }}.exe` + `dist/win-client/SHA256SUMS-${{ matrix.arch }}`，artifact name = `win-client-${{ matrix.arch }}`（两 leg 名字不同、绝不会互相覆盖），`if-no-files-found: error` 保证产物缺失时直接失败
   
   该 job 有意**不**写 `needs: build-web`——Windows 客户端不嵌 Web UI，与 `build-web` 并行启动省墙钟时间。**为什么 arm64 在 x64 runner 上交叉编译而不是用原生 arm64 runner**：GitHub Actions 截至本节写就时尚未提供公开的 Windows arm64 runner，`amd64_arm64` 交叉工具集是唯一可行方案；等未来 `windows-2022-arm64` 或类似 label 就绪，可把 arm64 leg 改成原生 runner + `arch: arm64`，删掉交叉编译路径
5. **`detect-docker-creds`**（只有 3 行：读 `secrets.DOCKERHUB_USERNAME` 是否非空，输出 `have=true/false`；存在是因为 `secrets.*` 不能直接用在 `if:` 表达式里）
6. **`build-docker`**（`needs: [build-web, detect-docker-creds]`、`if: needs.detect-docker-creds.outputs.have == 'true'`，用 `docker/setup-qemu-action` + `docker/setup-buildx-action` 推 `linux/amd64,linux/arm64,linux/arm/v7` 多架构）
7. **`publish-release`**（`needs: [build-server, build-mac-dmg, build-win-client, build-docker]`；条件是多行 `if:` 表达式，容忍 `build-docker` 在没 secrets 时被 `skipped`，但 server / mac / win 任一失败仍会中止——**矩阵 job 的 `.result` 是所有 leg 的聚合**，所以 x64 / arm64 任一失败都会让 `build-win-client.result` 变成 `failure` 从而阻塞 release；用 `softprops/action-gh-release@v2` 把 `build-server` / `build-mac-dmg` / `build-win-client`（两次 download：`win-client-x64` + `win-client-arm64`，都下到同一个 `dist/win-client/` 目录；arch 后缀保证 zip / setup.exe / SHA256SUMS 三类文件不冲突）的 artifact 挂到 Release）

### `release-tag.yml`（`on: push: tags: ['v*']`）

调用 `_build-all.yml`，`docker_image=docker.io/hanxi/stickytodo`、`tag_latest=true`、正式发布。

### `release-branch.yml`（`on: workflow_dispatch`，带 `branch` 输入）

先跑 `cleanup-old-release` job，**三阶段兜底**删同名旧 release（①`gh release delete --cleanup-tag` → ②降级为 `gh release delete` + `git push --delete origin <tag>` → ③容忍 tag/release 都不存在的首次运行），再调 `_build-all.yml` 生成 prerelease，`tag_latest=false` 确保不会覆盖 `:latest` 镜像。

### Secrets

所需 secrets：`DOCKERHUB_USERNAME` / `DOCKERHUB_TOKEN`。**不是"不配就跳过 push"，而是"不配就完全跳过 `build-docker` 这个 job"**（镜像不会构建、不会推送）；其他产物不受影响。完整手册见 [RELEASE.md](RELEASE.md)。

---

## 产物矩阵

| 产物类型 | 命名 | 备注 |
|---|---|---|
| Server 二进制 | `stickytodo-server-<ver>-<os>-<arch>[.exe]` | 7 份：linux × (amd64/arm64/armv7)、darwin × (amd64/arm64)、windows × (amd64/arm64)；与同目录的 `SHA256SUMS` 汇总文件一起上传 |
| Mac 客户端 | 发布文件名：`stickytodo-<ver>-macos-universal.dmg`；DMG 卷标（挂载后 Finder 窗口标题）：`stickytodo <ver>`；DMG 内 `.app` bundle：**`stickytodo.app`**（无版本号） | universal（arm64 + x86_64）；脚本用 `codesign --force --deep --options runtime --sign -` 做 **ad-hoc** 签名（`--sign -` 等价短写 `-s -`），`--options runtime` 启用 Hardened Runtime 以便将来可平滑切到开发者 ID 签名；同目录一份 `SHA256SUMS`。**三套命名的分工**：① 发布文件名要带 `<ver>` 供下载归档区分；② DMG 卷标要带 `<ver>` 方便用户知道自己挂的是哪个版本；③ `.app` bundle 名必须是干净的 `stickytodo.app`——这是用户拖进 `/Applications` 后在 Launchpad / Dock / Cmd+Tab 里永久看到的名字，绝不能混入发布文件名里的 `branch-main` / `v1.2.3` 之类噪声。历史 bug：曾把这三套名字合成一套，导致 DMG 双击开打是 "stickytodo branch-main (2849)" 这种脏窗口标题，且拖进 /Applications 后 App 图标下方显示 `stickytodo-branch-main-macos-universal`，非常不像正式应用 |
| Win 客户端（portable） | 发布文件名：`stickytodo-<ver>-windows-x64.zip` + `stickytodo-<ver>-windows-arm64.zip`（**无 `-portable` 后缀**，脚本里直接用干净命名）；zip 内顶层是 `stickytodo-<ver>-windows-<arch>/` 文件夹 → 内含 `stickytodo.exe`（无版本号——同 mac `.app` 的干净品牌名原则）+ 可选 `README.md` + 可选 `LICENSE.txt` | **双架构：x64（amd64）+ arm64**，两份独立产物，用户按自己系统下载对应包（Win11 arm64 系统即便能跑 x64 emulation 也推荐原生 arm64 版：更省电、性能更好）；**无代码签名**（首次启动会触发 SmartScreen 警告，用户需点击"更多信息 → 仍要运行"，这是开源项目可接受的 onboarding cost）；每架构一份 `SHA256SUMS-x64` / `SHA256SUMS-arm64` |
| Win 客户端（installer） | 发布文件名：`stickytodo-setup-<ver>-x64.exe` + `stickytodo-setup-<ver>-arm64.exe`（**`setup-<ver>-<arch>` 顺序**，Inno Setup 6 编译） | per-user 安装（默认不需 UAC；`DefaultDirName={autopf}\StickyTodo` + `PrivilegesRequired=lowest` + `PrivilegesRequiredOverridesAllowed=dialog commandline`，允许用户在安装向导里显式升级成 all-users 模式，此时 `{autopf}` 解析成 `{commonpf}`=`%ProgramFiles%\StickyTodo`）；`MinVersion=10.0.19041` 把可安装系统卡在 Windows 10 20H1+；**架构门禁**：x64 安装器 `ArchitecturesAllowed=x64compatible`（允许 native x64 + Win11-arm64 上的 x64 emulation 作为 fallback），arm64 安装器 `ArchitecturesAllowed=arm64`（**仅允许真实 arm64 host**，不接受 emulation，保证 arm64 二进制只落在真能原生运行它的系统上）；可选创建桌面快捷方式（Task `desktopicon` 带 `Flags: unchecked`，安装向导里**默认不勾**）；卸载器通过"设置 → 应用 → 已安装的应用"找到，arm64 版的 `UninstallDisplayName` 是 `StickyTodo (arm64)` 便于用户区分当前装的是哪个架构（x64 版保持 `StickyTodo` 无后缀）；`AppId` 是固定 GUID `{{4B5B6C2E-9E7B-4F3D-A8C5-0D6A1B2C3D4E}}` **两架构共用**（frozen，绝不变）——故意设计成共享升级通道：同一主机上若先装 x64 再装 arm64（或反向）会自动替换，而不是并存两份（单台机器不存在两架构都需要的合理场景）；每架构一份 `SHA256SUMS-<arch>` 与 portable zip 合用 |
| Docker 镜像 | `docker.io/hanxi/stickytodo:<ver>`（正式 tag 时还会打 `:latest`）| 多架构 manifest：`linux/amd64` / `linux/arm64` / `linux/arm/v7`；镜像分发**不带** SHA256SUMS，完整性靠 registry digest |

`SHA256SUMS` 由 `package-server.sh` / `package-mac-client.sh` / `package-win-client.sh` 在结尾处统一生成：优先 `sha256sum`（Linux、windows-2022 Git Bash 自带），缺失时 fallback `shasum -a 256`（macOS 自带），产物文件名以相对路径写入同目录的 `SHA256SUMS`。Docker 镜像没有也**不应该**有这个文件——镜像完整性靠 registry 返回的 content digest（`sha256:...`）校验。

---

## 交叉编译纪律

- 后端绝对不要引入需要 CGO 的依赖（例如原 `mattn/go-sqlite3`），`go.mod` review 时要看一眼
- Dockerfile 必须保持 `CGO_ENABLED=0`（静态链接）。运行阶段当前用 `FROM alpine:3.20`（见 Dockerfile 中唯一一个非 `--platform=$BUILDPLATFORM` 的 `FROM ... AS runtime` 行），容器内默认 root 跑——刻意选择，目的是让宿主机 bind-mount 的 `./data/` 不需要 `chown` 预处理即可写入（distroless+nonroot uid=65532 的方案在原生 Linux 主机上会出现 `attempt to write a readonly database` 错误）。运行阶段只装 `ca-certificates` + `tzdata`，不装其他系统包。如果未来要切回非 root 运行，需要在 entrypoint 里加 `chown` 兜底脚本，并同步更新 `docker-compose.yml` 中关于权限的注释和 `server/README.md` 的"镜像特性"章节
- Mac 客户端打 DMG 必须 universal——`package-mac-client.sh` 里同时传 `ARCHS="arm64 x86_64"` **和** `ONLY_ACTIVE_ARCH=NO`（两者必须成对，只传 ARCHS 不够；脚本最后还会 `lipo -archs` 核对产物确为 `arm64 + x86_64` fat binary），否则 Intel Mac 用户会拿不到可执行的 App
- Windows 客户端**同时打 x64（amd64）+ arm64**，两份独立产物。`CMakePresets.json` 里两架构各有一对 configure preset：`release` + `debug`（x64，绑 `x64-windows` triplet）/ `release-arm64` + `debug-arm64`（arm64，绑 `arm64-windows` triplet）。CI `build-win-client` job 用 `strategy.matrix.arch: [x64, arm64]` 并行两份，`ilammy/msvc-dev-cmd@v1` 的 arch 参数表达式 `matrix.arch == 'arm64' && 'amd64_arm64' || 'amd64'` 切换交叉工具集。**arm64 是 x64 runner 上的交叉编译**——binary 能构建、但不能在 x64 host 上执行，所以 `package-win-client.sh` 在 `ARCH=arm64` 时**无条件跳过 ctest**（codec / models 测试是纯 C++ 逻辑，x64 leg 已覆盖）。产物命名 `stickytodo-<ver>-windows-<x64|arm64>.zip` + `stickytodo-setup-<ver>-<x64|arm64>.exe`，**不能**把两架构塞进同一 installer 混合分发——Inno Setup 的 `ArchitecturesAllowed` 按架构 gating（x64 用 `x64compatible`、arm64 用 `arm64`），AppId GUID 共用以维持单一升级通道。未来若 GitHub Actions 提供原生 Windows arm64 runner（如 `windows-2022-arm64`），可把 arm64 leg 迁到原生 runner + `arch: arm64`，ctest 就能跑起来，删掉 `package-win-client.sh` 里 "arm64 跳过 ctest" 的分支
- Windows 客户端**严禁**引入需要 MinGW 工具链的依赖——CI 和本机都走 MSVC（MSBuild / cl.exe），vcpkg 默认 triplet 是 `x64-windows`（dynamic CRT）。如果某个库只在 `x64-mingw-dynamic` 有，第一反应应是找 MSVC 替代，而不是切 triplet（会连锁触发所有其它 vcpkg 依赖的重建）

---

## 版本号来源

- CI 里版本来自 `github.ref_name`（tag 名）
- 本地脚本来自 `$VERSION` 环境变量，`package-server.sh` / `package-mac-client.sh` / `package-docker.sh` 均默认 `dev`；`package-web.sh` 不读 `VERSION`（静态产物）
- 后端二进制启动时 `/health` 返回的 `version` 由 `-ldflags "-X main.version=..."` 在 build 时注入，用户能实时看到
- **Mac 客户端版本号的限制**：当前 `MARKETING_VERSION` 在 `stickytodo.xcodeproj/project.pbxproj` 里**硬编码为 `1.0`**，`package-mac-client.sh` 不会修改 Info.plist，因此 DMG 里的 App "关于"信息永远显示 `1.0`；外部可见的版本号只有**产物文件名**（`stickytodo-<VERSION>-macos-universal.dmg`）。如果未来需要把 `$VERSION` 真正写进 App Bundle，需要在 `package-mac-client.sh` 的 xcodebuild 阶段额外改 pbxproj 的 `MARKETING_VERSION` 或用 `PlistBuddy` 改生成后的 `*.app/Contents/Info.plist`
- **Windows 客户端版本号的限制**：当前 `app.rc` 里的 `FILEVERSION` / `PRODUCTVERSION` **硬编码为 `1,0,0,0`**，与 mac 客户端同 corner case，详见上方 `package-win-client.sh` 表格行

---

## macOS 客户端 Xcode / SDK 版本一致性

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

### 决策历史

每一步都要写清"为什么上一步不够"，避免后人重复踩坑：

1. **阶段 1（`macos-14` + Xcode 15.4）**：CI 和本机 Xcode 跨主版本，SwiftUI `.buttonStyle(.bordered)` 默认外观填充差异明显——便签加号按钮在 CI DMG 里是**白底**、本机 Debug 是**浅灰底**。典型跨 SDK 视觉漂移
2. **阶段 2（尝试：`runs-on: macos-latest` + `setup-xcode 26.4`）**：**本阶段基于多个错误前提**：
   - 错 ①：以为 `macos-latest` 指向 macos-26 runner（实际指向 macos-15-arm64，上面完全没有 Xcode 26.x）
   - 错 ②：以为 setup-xcode "必须精确字面量匹配"（实际支持 SemVer）
   
   失败日志：`Could not find Xcode version that satisfied version spec: '26.4'`。**真正原因**是 macos-15-arm64 runner 上就没装任何 Xcode 26.x，setup-xcode 在 `/Applications/` 下找不到任何能满足 `26.4` 的 app bundle——和字面量/SemVer 无关
3. **阶段 3（尝试：`runs-on: macos-latest` + 不锁 Xcode + 只靠源码层 `.tint(.secondary)` + `.controlSize(.small)`）**：误把问题归因到"runner 默认 Xcode 和本机只差一点"，想靠源码层兜底。实测 CI 产出的加号仍是白底、本机灰底。**真正原因**依然是 `macos-latest` = macos-15 runner 根本没有 macOS 26 SDK，产物其实是 macOS 15 SDK（Xcode 16.x）编出来的，和本机 macOS 26.4 SDK 跨了整整一个主版本——源码层 modifier 覆盖不住这么大的 SDK 差
4. **阶段 4（当前：`runs-on: macos-26` + `setup-xcode 26.4.1` + 保留源码层 modifier）**：真正把 runner 从 macos-15 切到 macos-26（这才有 macOS 26 SDK 可选），再锁 Xcode 26.4.1 对齐本机。锁 runner + 锁 Xcode = 第一道防线（SDK 对齐），源码层 modifier = 第二道防线

### 如何升级 / 调整锁定的 Xcode 版本

1. 打开 `https://github.com/actions/runner-images/blob/main/images/macos/macos-26-arm64-Readme.md`（如果目标是 macos-27 GA，就去 `macos-27-arm64-Readme.md`），找到 `### Xcode` 的表格
2. 从表格挑一个**字面量**——可以是主版本号（`26.5`）或完整修订号（`26.4.1`）；由于 setup-xcode 支持 SemVer，如果想写 `26.4` 也合法（会解析到 `Xcode_26.4.1.app` 的 symlink）。不过推荐字面量完整版本号（`26.4.1`）以便一眼看出和本机对齐的是哪个修订
3. 同时改 3 处（全部改完才算一次升级完整）：
   - `.github/workflows/_build-all.yml` 的 `- name: Select Xcode ...` step 的 `xcode-version` 字面量
   - `.github/workflows/_build-all.yml` 的 `build-mac-dmg` 注释块里"预装列表"的版本号快照
   - 本文档"当前策略"+ "关键事实"两处版本号
4. 如果 runner 镜像本身要升（比如 macOS 27 GA 后想迁到 macos-27），那还要把 `runs-on` 同步改，且"关键事实"段落里的 runner-images README 引用也要改

### 仍然要守的纪律

1. **`runs-on` 必须显式写 `macos-26`，绝不能改回 `macos-latest`**。已经踩过两次坑，`-latest` 不等于"最新 macOS"。即使未来 `macos-latest` 有一天迁到 macos-26 了，也要等迁移稳定、且本文档确认过之后才能改
2. **CI 侧的 `setup-xcode` step 不能删**。只依赖 runner 默认 Xcode（`macos-26` 默认是 26.2）会与本机 26.4 跨两个次要版本，`.buttonStyle(.bordered)` 的填充基色会漂
3. **源码层的显式 modifier 也不能删**。当前至少 `StickyView.swift` 的加号按钮依赖 `.tint(.secondary)` + `.controlSize(.small)`。未来如果发现 `MenuBarContent.swift` / `SettingsView.swift` 里的 `.bordered` / `.borderedProminent` / `.menuStyle(.borderlessButton)` 在 CI 产物和本机有分歧，第一反应应是**再给它们也加显式外观 modifier**
4. **本机 Xcode 不要比 CI 锁的版本超前太多**。当前约束：本机 Xcode 主次版本 ≤ 锁定版本的主次版本 + 1。例如锁 26.4.1，本机用 26.4 / 26.5 都可接受；本机升到 27.x 就必须先把 CI 锁定版同步升级（前提：runner 预装列表里有对应字面量），否则 CI 和本机再次跨 SDK 漂移
5. **runner 镜像更新后的抽检**：runner-images 大约每 2-3 周 release 一次，有时会在 minor release 里轮转掉老 Xcode。建议每月手动跑一次 `release-branch` workflow，挂载产出的 DMG、肉眼看一眼加号 / 其它关键按钮的外观；如果发现 setup-xcode step 开始报 `Could not find Xcode version` 错误，立刻到 runner-images readme 里查最新预装列表，按上面"如何升级"的流程更新
