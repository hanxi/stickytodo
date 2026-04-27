#!/usr/bin/env bash
#
# package-server.sh — Cross-compile the stickytodo-server binary for 7 platforms
# and emit sha256 checksums.
#
# Matrix (7 artifacts):
#   linux/amd64        linux/arm64        linux/arm (GOARM=7)
#   darwin/amd64       darwin/arm64
#   windows/amd64      windows/arm64
#
# Prerequisite: the web bundle must be synced into server/internal/webui/dist
# before `go build`, otherwise go:embed will bake an empty directory into the
# binary. This script calls scripts/package-web.sh automatically unless
# --skip-web is passed.
#
# Inputs:
#   env VERSION         Build tag injected as -X main.version (default: dev)
#   flag --skip-web     Skip the web build step (assumes embed dir is already synced)
#   flag --platforms    Comma-separated subset of "os/arch[/arm_version]" targets.
#                       Default: all 7. Useful for local quick builds.
#                       Examples:
#                         --platforms linux/amd64
#                         --platforms linux/amd64,darwin/arm64
#
# Outputs (under dist/server/):
#   stickytodo-server-${VERSION}-linux-amd64
#   stickytodo-server-${VERSION}-linux-arm64
#   stickytodo-server-${VERSION}-linux-armv7
#   stickytodo-server-${VERSION}-darwin-amd64
#   stickytodo-server-${VERSION}-darwin-arm64
#   stickytodo-server-${VERSION}-windows-amd64.exe
#   stickytodo-server-${VERSION}-windows-arm64.exe
#   SHA256SUMS             (one line per artifact, relative paths)
#
# Notes:
#   - CGO_ENABLED=0 everywhere (we migrated to github.com/glebarez/sqlite).
#   - -trimpath + -ldflags "-s -w" shrinks binaries and removes absolute paths.
#   - Each build runs in parallel-friendly fashion but we execute serially to
#     keep logs readable and avoid hammering the local machine.
#
# Usage:
#   VERSION=v1.2.3 scripts/package-server.sh
#   scripts/package-server.sh --platforms linux/amd64

set -euo pipefail

# ---------- locate repo root ----------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

SERVER_DIR="$REPO_ROOT/server"
OUT_DIR="$REPO_ROOT/dist/server"

# ---------- flags ----------
SKIP_WEB=0
PLATFORMS_OVERRIDE=""
for arg in "$@"; do
  case "$arg" in
    --skip-web) SKIP_WEB=1 ;;
    --platforms=*) PLATFORMS_OVERRIDE="${arg#--platforms=}" ;;
    --platforms)
      echo "package-server: --platforms requires a value (use --platforms=linux/amd64 or quote it)" >&2
      exit 2
      ;;
    -h|--help)
      sed -n '2,42p' "$0"
      exit 0
      ;;
    *)
      # Allow positional form: --platforms linux/amd64
      if [[ -z "$PLATFORMS_OVERRIDE" && "$arg" != --* && "$arg" == */* ]]; then
        PLATFORMS_OVERRIDE="$arg"
      else
        echo "package-server: unknown flag: $arg" >&2
        exit 2
      fi
      ;;
  esac
done

VERSION="${VERSION:-dev}"

log() { printf '[package-server] %s\n' "$*"; }

# ---------- preflight ----------
if ! command -v go >/dev/null 2>&1; then
  echo "package-server: go is required but not found in PATH" >&2
  exit 1
fi
if [[ ! -f "$SERVER_DIR/go.mod" ]]; then
  echo "package-server: $SERVER_DIR/go.mod missing" >&2
  exit 1
fi

log "go=$(go version | awk '{print $3}') VERSION=${VERSION}"

# ---------- web build (unless skipped) ----------
if [[ "$SKIP_WEB" -eq 1 ]]; then
  log "skip-web: assuming $SERVER_DIR/internal/webui/dist is already populated"
  if [[ ! -f "$SERVER_DIR/internal/webui/dist/index.html" ]]; then
    echo "package-server: --skip-web requested but embed dir has no index.html" >&2
    echo "package-server: run scripts/package-web.sh first" >&2
    exit 1
  fi
else
  log "running scripts/package-web.sh to refresh embed dir"
  bash "$SCRIPT_DIR/package-web.sh"
fi

# ---------- target matrix ----------
# Full matrix. Each entry is "os/arch[/armv]".
# Windows on arm doesn't have an arm_version concept, only arm64 is supported.
ALL_PLATFORMS=(
  "linux/amd64"
  "linux/arm64"
  "linux/arm/7"
  "darwin/amd64"
  "darwin/arm64"
  "windows/amd64"
  "windows/arm64"
)

# Resolve effective platform list.
declare -a PLATFORMS
if [[ -n "$PLATFORMS_OVERRIDE" ]]; then
  IFS=',' read -r -a PLATFORMS <<<"$PLATFORMS_OVERRIDE"
else
  PLATFORMS=("${ALL_PLATFORMS[@]}")
fi

log "platforms: ${PLATFORMS[*]}"

# ---------- prepare output dir ----------
mkdir -p "$OUT_DIR"
# Remove previous artifacts for this VERSION — keep other VERSION outputs
# so multiple builds can coexist when the caller wants to diff.
find "$OUT_DIR" -maxdepth 1 -type f -name "stickytodo-server-${VERSION}-*" -delete
rm -f "$OUT_DIR/SHA256SUMS"

cd "$SERVER_DIR"

# ---------- build loop ----------
BUILT_ARTIFACTS=()
for target in "${PLATFORMS[@]}"; do
  # Parse "os/arch[/armv]".
  IFS='/' read -r os arch armv <<<"$target"
  if [[ -z "$os" || -z "$arch" ]]; then
    echo "package-server: invalid platform: $target" >&2
    exit 2
  fi

  # Artifact filename convention: linux/arm/7 → linux-armv7.
  label_arch="$arch"
  if [[ "$arch" == "arm" && -n "$armv" ]]; then
    label_arch="armv${armv}"
  fi

  ext=""
  if [[ "$os" == "windows" ]]; then
    ext=".exe"
  fi

  artifact_name="stickytodo-server-${VERSION}-${os}-${label_arch}${ext}"
  artifact_path="$OUT_DIR/$artifact_name"

  log "building ${os}/${arch}${armv:+/armv$armv} → ${artifact_name}"

  # Use `env` to apply environment variables only to the go build process
  # without leaking them into the outer shell or being interpreted as commands.
  env_args=(CGO_ENABLED=0 "GOOS=$os" "GOARCH=$arch")
  if [[ "$arch" == "arm" && -n "$armv" ]]; then
    env_args+=("GOARM=$armv")
  fi

  env "${env_args[@]}" go build \
    -trimpath \
    -ldflags "-s -w -X main.version=${VERSION}" \
    -o "$artifact_path" \
    ./cmd/todo-server

  if [[ ! -f "$artifact_path" ]]; then
    echo "package-server: go build completed but $artifact_path is missing" >&2
    exit 1
  fi

  # Size for the log line (human-readable).
  if command -v du >/dev/null 2>&1; then
    size="$(du -h "$artifact_path" | awk '{print $1}')"
    log "  → ${artifact_name} (${size})"
  fi

  BUILT_ARTIFACTS+=("$artifact_name")
done

# ---------- checksums ----------
log "generating SHA256SUMS"
cd "$OUT_DIR"
# Use the portable -a256 if available (macOS shasum), fall back to sha256sum.
if command -v sha256sum >/dev/null 2>&1; then
  sha256sum "${BUILT_ARTIFACTS[@]}" > SHA256SUMS
elif command -v shasum >/dev/null 2>&1; then
  shasum -a 256 "${BUILT_ARTIFACTS[@]}" > SHA256SUMS
else
  echo "package-server: neither sha256sum nor shasum found; cannot generate checksums" >&2
  exit 1
fi

log "artifacts in $OUT_DIR:"
ls -la "$OUT_DIR" | sed 's/^/  /'

log "SHA256SUMS preview:"
sed 's/^/  /' SHA256SUMS

log "done. ${#BUILT_ARTIFACTS[@]} artifacts produced."
