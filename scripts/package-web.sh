#!/usr/bin/env bash
#
# package-web.sh — Build the Vite web bundle and sync it into the Go embed target.
#
# Inputs:
#   - client/web/              (React + Vite source, base: '/app/')
#
# Outputs:
#   - client/web/dist/                       (Vite build output)
#   - server/internal/webui/dist/            (synced copy, consumed by go:embed)
#     * Preserves .gitkeep so the embed directive always has a target
#     * Overwrites any previous build artifacts
#
# Usage (from repo root or anywhere inside the repo):
#   scripts/package-web.sh                   # full build
#   scripts/package-web.sh --skip-install    # reuse node_modules (CI already cached)
#
# Exit 0 on success; non-zero on any step failure.
#
# Idempotent: running twice yields the same artifact. Safe to re-run after
# partial failures.

set -euo pipefail

# ---------- locate repo root ----------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

WEB_DIR="$REPO_ROOT/client/web"
EMBED_DIR="$REPO_ROOT/server/internal/webui/dist"

# ---------- flags ----------
SKIP_INSTALL=0
for arg in "$@"; do
  case "$arg" in
    --skip-install) SKIP_INSTALL=1 ;;
    -h|--help)
      sed -n '2,24p' "$0"
      exit 0
      ;;
    *)
      echo "package-web: unknown flag: $arg" >&2
      exit 2
      ;;
  esac
done

log() { printf '[package-web] %s\n' "$*"; }

# ---------- preflight ----------
if ! command -v node >/dev/null 2>&1; then
  echo "package-web: node is required but not found in PATH" >&2
  exit 1
fi
if ! command -v npm >/dev/null 2>&1; then
  echo "package-web: npm is required but not found in PATH" >&2
  exit 1
fi
if [[ ! -f "$WEB_DIR/package.json" ]]; then
  echo "package-web: $WEB_DIR/package.json missing — is this the stickytodo repo?" >&2
  exit 1
fi

log "node=$(node -v) npm=$(npm -v)"
log "web dir:   $WEB_DIR"
log "embed dir: $EMBED_DIR"

# ---------- install ----------
cd "$WEB_DIR"
if [[ "$SKIP_INSTALL" -eq 1 ]]; then
  log "skip-install: reusing $WEB_DIR/node_modules"
  if [[ ! -d node_modules ]]; then
    echo "package-web: --skip-install requested but node_modules/ does not exist" >&2
    exit 1
  fi
else
  if [[ -f package-lock.json ]]; then
    log "npm ci (lockfile present)"
    npm ci
  else
    log "npm install (no lockfile)"
    npm install
  fi
fi

# ---------- build ----------
log "vite build"
npm run build

if [[ ! -f "$WEB_DIR/dist/index.html" ]]; then
  echo "package-web: vite build finished but $WEB_DIR/dist/index.html is missing" >&2
  exit 1
fi

# ---------- sync to embed dir ----------
# Strategy: wipe everything in $EMBED_DIR EXCEPT .gitkeep, then copy fresh.
# Avoid `rm -rf $EMBED_DIR` (which would nuke the directory itself and potentially
# confuse `go:embed all:dist` until the directory is recreated).
mkdir -p "$EMBED_DIR"
log "clearing $EMBED_DIR (keeping .gitkeep)"
find "$EMBED_DIR" -mindepth 1 -not -name '.gitkeep' -exec rm -rf {} +

log "syncing $WEB_DIR/dist → $EMBED_DIR"
# Use `cp -R src/. dest/` so the CONTENTS of dist/ end up directly under EMBED_DIR,
# not a nested dist/ sub-folder. The trailing `/.` is a POSIX idiom.
cp -R "$WEB_DIR/dist/." "$EMBED_DIR/"

# ---------- sanity ----------
if [[ ! -f "$EMBED_DIR/index.html" ]]; then
  echo "package-web: sync failed — $EMBED_DIR/index.html not found after copy" >&2
  exit 1
fi

# Print a short manifest so CI logs show exactly what was produced.
log "artifact listing:"
( cd "$EMBED_DIR" && find . -maxdepth 3 -type f | sort | sed 's/^/  /' )

log "done."
