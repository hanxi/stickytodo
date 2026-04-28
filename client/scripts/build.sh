#!/usr/bin/env bash
#
# client/scripts/build.sh
#
# stickytodo（macOS 客户端）的一键 clean + build 回归脚本。
# 用于本地/CI 验证工程完整性与编译正确性。
#
# 成功 → 退出码 0
# 失败 → 退出码非 0 并在 stderr 打印最后若干行 xcodebuild 错误
#
# 用法:
#   ./client/scripts/build.sh           # Debug 构建（默认）
#   CONFIG=Release ./client/scripts/build.sh
#

set -euo pipefail

# 切到仓库根（脚本自身位于 client/scripts）
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
PROJECT_DIR="${REPO_ROOT}/client/mac"
PROJECT_FILE="${PROJECT_DIR}/stickytodo.xcodeproj"
SCHEME="stickytodo"
CONFIG="${CONFIG:-Debug}"
DERIVED_DATA="${DERIVED_DATA:-/tmp/stickytodoBuild}"

if [[ ! -d "${PROJECT_FILE}" ]]; then
  echo "[build.sh] ERROR: 未找到 Xcode 工程: ${PROJECT_FILE}" >&2
  exit 1
fi

if ! command -v xcodebuild >/dev/null 2>&1; then
  echo "[build.sh] ERROR: 当前环境未安装 xcodebuild（需要 Xcode Command Line Tools）" >&2
  exit 1
fi

echo "[build.sh] 项目根: ${REPO_ROOT}"
echo "[build.sh] 配置  : ${CONFIG}"
echo "[build.sh] 派生  : ${DERIVED_DATA}"
echo

# 清理旧派生目录，避免"陈旧产物假通过"
rm -rf "${DERIVED_DATA}"

# 日志先落盘再按需展示，便于失败时定位。
# 直接指定模板路径以规避 BSD/GNU `mktemp -t` 语义差异：
#   - BSD (macOS) 默认 mktemp 创建的是真实 0600 权限空文件，我们再把 xcodebuild
#     的输出用 `>` 覆盖写入；
#   - EXIT trap 负责清理该临时文件，包括正常退出、set -e 中断、显式 exit 三种路径。
LOG_FILE="$(mktemp /tmp/stickytodo-build.XXXXXX)"
trap 'rm -f "${LOG_FILE}"' EXIT

set +e
xcodebuild \
  -project "${PROJECT_FILE}" \
  -scheme "${SCHEME}" \
  -configuration "${CONFIG}" \
  -destination 'platform=macOS' \
  -derivedDataPath "${DERIVED_DATA}" \
  CODE_SIGN_IDENTITY="-" \
  CODE_SIGNING_REQUIRED=NO \
  CODE_SIGNING_ALLOWED=NO \
  clean build \
  > "${LOG_FILE}" 2>&1
BUILD_EXIT=$?
set -e

if [[ ${BUILD_EXIT} -ne 0 ]]; then
  echo "[build.sh] xcodebuild 失败 (exit=${BUILD_EXIT})，最后 60 行日志：" >&2
  tail -n 60 "${LOG_FILE}" >&2
  exit ${BUILD_EXIT}
fi

# 再次确认最后一行是 BUILD SUCCEEDED，避免 xcodebuild 退出码为 0 但实际上失败
if ! grep -q '\*\* BUILD SUCCEEDED \*\*' "${LOG_FILE}"; then
  echo "[build.sh] 未检测到 BUILD SUCCEEDED，判定为失败。最后 60 行日志：" >&2
  tail -n 60 "${LOG_FILE}" >&2
  exit 1
fi

# 产物路径
APP_PATH="${DERIVED_DATA}/Build/Products/${CONFIG}/stickytodo.app"
if [[ -d "${APP_PATH}" ]]; then
  echo "[build.sh] ✅ BUILD SUCCEEDED"
  echo "[build.sh]    产物: ${APP_PATH}"
else
  echo "[build.sh] WARN: BUILD SUCCEEDED 但未找到 .app 产物（${APP_PATH}）" >&2
  exit 1
fi
