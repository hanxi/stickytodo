# syntax=docker/dockerfile:1.6
#
# Build context: repository root (not server/).
#   docker build -f server/Dockerfile -t stickytodo .
#
# Rationale:
#   - We need BOTH client/web/ (to build the web UI) and server/ (to embed it)
#     inside the build context. Hence context = repo root.
#   - Pure-Go SQLite driver (github.com/glebarez/sqlite) removes the CGO
#     requirement, so we can cross-compile statically and target a distroless
#     runtime image for minimal attack surface.
#   - Multi-arch via buildx: TARGETOS / TARGETARCH / TARGETVARIANT are provided
#     automatically by `docker buildx build --platform`.

# ---------- Stage 1: web builder ----------
FROM --platform=$BUILDPLATFORM node:20-alpine AS web-builder

WORKDIR /web

# Install deps first to leverage layer cache.
# package-lock.json is committed to the repo (see client/web/package-lock.json),
# so we COPY both files explicitly and always use `npm ci` for reproducibility.
COPY client/web/package.json client/web/package-lock.json ./
RUN --mount=type=cache,target=/root/.npm \
    npm ci

# Build the web bundle. Vite writes to ./dist with base '/app/'.
COPY client/web/ ./
RUN npm run build

# ---------- Stage 2: go builder ----------
FROM --platform=$BUILDPLATFORM golang:1.25-alpine AS go-builder

# Pure-Go SQLite (no CGO). Produces fully static binaries portable across
# alpine / distroless / scratch runtimes.
ENV CGO_ENABLED=0 \
    GOFLAGS=-trimpath

RUN apk add --no-cache git ca-certificates tzdata

WORKDIR /src

# Cache module downloads first.
COPY server/go.mod server/go.sum ./
RUN --mount=type=cache,target=/go/pkg/mod \
    go mod download

# Copy server sources and the prebuilt web bundle into the embed target.
COPY server/ ./
COPY --from=web-builder /web/dist/ ./internal/webui/dist/

ARG VERSION=dev
ARG TARGETOS
ARG TARGETARCH
ARG TARGETVARIANT

# Map Docker's TARGETVARIANT (e.g. "v7", "v6") to Go's GOARM env.
RUN set -eu; \
    goarm=""; \
    case "${TARGETVARIANT:-}" in \
      v5) goarm="5" ;; \
      v6) goarm="6" ;; \
      v7) goarm="7" ;; \
    esac; \
    export GOOS="${TARGETOS}" GOARCH="${TARGETARCH}" GOARM="${goarm}"; \
    echo "[go-builder] GOOS=${GOOS} GOARCH=${GOARCH} GOARM=${GOARM} VERSION=${VERSION}"; \
    go build \
        -ldflags "-s -w -X main.version=${VERSION}" \
        -o /out/todo-server \
        ./cmd/todo-server

# Pre-create /data with uid=65532 (distroless nonroot user) so the runtime
# image has a writable data directory even when no host volume is mounted.
# distroless/static has no shell, so we cannot `mkdir + chown` in the final
# stage — must prepare the directory here and COPY it across.
RUN mkdir -p /out/data && chown -R 65532:65532 /out/data

# ---------- Stage 3: runtime ----------
# distroless/static:nonroot — minimal image, non-root (uid 65532), ships
# ca-certificates + tzdata + /etc/passwd. No shell, no package manager.
FROM gcr.io/distroless/static-debian12:nonroot AS runtime

ENV TODO_PORT=8080 \
    TODO_DATA_DIR=/data \
    TODO_GIN_MODE=release

WORKDIR /app

COPY --from=go-builder --chown=nonroot:nonroot /out/todo-server /app/todo-server
# /data is pre-created with nonroot ownership in the builder stage.
# Without this, the anonymous volume Docker auto-mounts on VOLUME ["/data"]
# would be owned by root and the nonroot process would fail with
# SQLITE_CANTOPEN (code 14).
COPY --from=go-builder --chown=nonroot:nonroot /out/data /data

VOLUME ["/data"]

EXPOSE 8080

ENTRYPOINT ["/app/todo-server"]
