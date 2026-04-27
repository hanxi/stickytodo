#!/usr/bin/env bash
#
# package-docker.sh — Build a stickytodo Docker image for the current host
# platform only, loading it into the local daemon so you can immediately
# `docker run` it.
#
# Multi-arch (linux/amd64 + linux/arm64 + linux/arm/v7) builds are intentionally
# handled by the GitHub Actions release workflow (see .github/workflows/), not
# by this script — cross-arch emulation via QEMU is slow locally and the CI
# runner ecosystem does it for free.
#
# Inputs:
#   env VERSION   Image tag (default: dev)
#   env IMAGE     Image name (default: stickytodo)
#
# Outputs:
#   Local docker image: ${IMAGE}:${VERSION}
#
# Usage:
#   scripts/package-docker.sh
#   VERSION=v1.2.3 IMAGE=stickytodo scripts/package-docker.sh

set -euo pipefail

# ---------- locate repo root ----------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

DOCKERFILE="$REPO_ROOT/server/Dockerfile"

VERSION="${VERSION:-dev}"
IMAGE="${IMAGE:-stickytodo}"

# ---------- flags ----------
for arg in "$@"; do
  case "$arg" in
    -h|--help)
      sed -n '2,22p' "$0"
      exit 0
      ;;
    *)
      echo "package-docker: unknown flag: $arg" >&2
      echo "package-docker: this script has no options; multi-arch is handled by CI" >&2
      exit 2
      ;;
  esac
done

log() { printf '[package-docker] %s\n' "$*"; }

# ---------- preflight ----------
if ! command -v docker >/dev/null 2>&1; then
  echo "package-docker: docker is required but not found in PATH" >&2
  exit 1
fi
if [[ ! -f "$DOCKERFILE" ]]; then
  echo "package-docker: $DOCKERFILE not found" >&2
  exit 1
fi

log "VERSION=${VERSION}"
log "IMAGE=${IMAGE}"

# ---------- build ----------
# `docker build` (not `docker buildx build --load`) is enough for a single
# host-arch image and avoids buildx driver quirks on colima/Docker Desktop.
cd "$REPO_ROOT"
log "docker build ..."
docker build \
  -f "$DOCKERFILE" \
  --build-arg "VERSION=${VERSION}" \
  -t "${IMAGE}:${VERSION}" \
  .

log "done. Image loaded into local daemon:"
docker images "${IMAGE}" --format "  {{.Repository}}:{{.Tag}} {{.Size}} {{.CreatedSince}}" | head -5
