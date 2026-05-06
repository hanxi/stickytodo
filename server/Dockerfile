# syntax=docker/dockerfile:1.6
#
# Build context: repository root (not server/).
#   docker build -f server/Dockerfile -t stickytodo .
#
# Rationale:
#   - We need BOTH client/web/ (to build the web UI) and server/ (to embed it)
#     inside the build context. Hence context = repo root.
#   - Pure-Go SQLite driver (github.com/glebarez/sqlite) removes the CGO
#     requirement, so we can cross-compile statically into any minimal runtime.
#   - Runtime is alpine (not distroless): the server runs as root by default,
#     which removes the bind-mount uid headache on native Linux hosts — the
#     `./data` directory bind-mounted from the host is writable by the
#     container without any pre-flight `chown 65532` step. Trade-off: the
#     in-container process is root; acceptable for a self-hosted single-tenant
#     todo app where the threat model is "this box only runs my own code".
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
# alpine / scratch runtimes.
ENV CGO_ENABLED=0 \
    GOFLAGS=-trimpath

# git + ca-certificates are needed by `go mod download` (HTTPS + VCS proto).
# tzdata lives only in the runtime stage where the server actually reads
# /usr/share/zoneinfo for the TZ env, so it is not installed here.
RUN apk add --no-cache git ca-certificates

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

# ---------- Stage 3: runtime ----------
# alpine:3.20 — has a real shell + apk and runs as root by default.
# ca-certificates + tzdata are installed explicitly: the static Go binary
# still needs the CA bundle for outbound HTTPS and a zoneinfo database for
# the TZ env (e.g. Asia/Shanghai).
FROM alpine:3.20 AS runtime

RUN apk add --no-cache ca-certificates tzdata

ENV TODO_PORT=8080 \
    TODO_DATA_DIR=/data \
    TODO_GIN_MODE=release

WORKDIR /app

COPY --from=go-builder /out/todo-server /app/todo-server

# /data is the SQLite data directory. Created here so it exists even when
# the operator forgets to bind-mount a host directory; when a host directory
# IS bind-mounted, this just gets shadowed and the host's permissions take
# effect. Running as root means we don't care about the host uid.
RUN mkdir -p /data

VOLUME ["/data"]

EXPOSE 8080

ENTRYPOINT ["/app/todo-server"]
