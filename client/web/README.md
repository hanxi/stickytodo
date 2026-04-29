# stickytodo Web Client

React + Vite + Tailwind single-page app. Mirrors the macOS menu-bar client's multi-sticky-note UX in the browser, talking to the same Go backend REST API.

## Tech stack

- **React 18** + **TypeScript** (strict, `tsc -b --noEmit` as typecheck gate)
- **Vite 5** — dev server + production build. `base: '/app/'` so bundled asset URLs line up with the Go embed mount (`server/internal/webui/webui.go`).
- **Tailwind CSS** for styling; dark mode via `document.documentElement.classList.toggle('dark', …)` driven by the UI store.
- **Zustand** (`+ persist` middleware) for client-owned state that outlives a page reload:
  - `store/authStore.ts` — JWT / username / expiry, persisted to `localStorage` key `stickytodo.auth`
  - `store/uiStore.ts` — dark-mode preference, persisted to `localStorage` key `stickytodo.ui`
  - > ⚠️ `store/stickyStore.ts` **has been removed** in the "cloud data source" refactor. Sticky notes are now a server resource (`/api/sticky-notes`) managed purely by TanStack Query — nothing sticky-related is persisted in `localStorage` anymore. See `AGENTS.md §4.1` and `§7.3` for the migration rationale.
- **@tanstack/react-query** for every server-owned resource (todos / tags / history / audit logs / **stickies**). Single `QueryClient` wired in `src/main.tsx` with `staleTime: 5s`, `retry: 1`, `refetchOnWindowFocus: false`. Query keys live in `src/api/queryKeys.ts` so invalidations stay DRY.
- **Native `WebSocket`** inside `src/api/ws.ts` — a singleton `stickyWS` client that owns one long-lived connection to `/api/ws`. Implements the backend's "first-frame auth" protocol (send `{type:'auth',token}` within 2s of the upgrade, wait for `{type:'ready'}`), `[1,2,4,8,16,30]s` exponential backoff, immediate reconnect on `visibilitychange → visible`, and `close code 4401 → 'unauthorized'` signal. `src/hooks/useRealtimeSync.ts` bridges emitted events to React Query cache invalidations.
- Native `fetch` inside `src/api/client.ts` — no axios. Throws `ApiError { status, message }` for non-2xx responses; 401 auto-clears the auth store so the next render falls back to the login view. Sticky-note methods internally go through `src/lib/stickyCodec.ts` to convert between the server's `{frame, bg_color, filter}` JSON-string fields and the UI-friendly `StickyView` shape.
- **date-fns** for formatting, **lucide-react** for icons, **clsx** for className composition.

## Local development

```bash
cd client/web
npm install
npm run dev        # http://127.0.0.1:5173
```

`vite.config.ts` proxies `/api/*` and `/health` to `http://127.0.0.1:8080` over **HTTP only** (no `ws: true`), so start the Go backend in a second terminal:

```bash
cd server && set -a && . ./.env && set +a && go run ./cmd/todo-server
```

Log in with `TODO_USERNAME` / `TODO_PASSWORD` from the server's `.env`.

> ⚠️ **WebSocket is not proxied in dev.** The proxy above intentionally omits `ws: true`, so `/api/ws` handshakes from `:5173` will fail at the Vite dev server. See [Realtime sync (WebSocket)](#realtime-sync-websocket) below for the two supported workarounds (direct connect or build-and-serve from the Go embed).

### Scripts

| Command | Purpose |
|---|---|
| `npm run dev`        | Vite dev server with HMR and API proxy |
| `npm run build`      | Production build → `client/web/dist/` |
| `npm run preview`    | Serve the built `dist/` locally for a final sanity check |
| `npm run typecheck`  | `tsc -b --noEmit` — matches the CI gate |

## Building for release

**Don't call `npm run build` directly** when preparing a release — use the repo-root helper so the output lands in the Go embed directory:

```bash
bash scripts/package-web.sh            # npm ci + vite build + sync to server/internal/webui/dist/
bash scripts/package-web.sh --skip-install   # reuse cached node_modules (CI fast path)
```

The helper:

1. Runs `npm ci` (or `npm install` if no lockfile) unless `--skip-install`.
2. Runs `npm run build`.
3. Wipes `server/internal/webui/dist/` (preserving `.gitkeep`) and copies the fresh `dist/` contents in via `cp -R dist/. .../dist/`.
4. Fails loudly if `index.html` doesn't land in the target directory.

The server's `internal/webui/webui.go` has `//go:embed all:dist` → those files get baked into the Go binary. No separate static-asset deployment.

## Project layout

```
client/web/
├── src/
│   ├── main.tsx                # ReactDOM root + QueryClientProvider
│   ├── App.tsx                 # auth gate: token? StickyBoard : LoginView;
│   │                           # owns dark-mode effect; mounts useRealtimeSync() once at the root
│   ├── index.css               # Tailwind entry
│   ├── api/
│   │   ├── client.ts           # fetch wrapper + ApiError + all REST calls (incl. stickies)
│   │   ├── queryKeys.ts        # centralized react-query cache keys (todos / tags / stickies / ...)
│   │   └── ws.ts               # stickyWS singleton: /api/ws connection, first-frame auth,
│   │                           # [1,2,4,8,16,30]s backoff, 4401 → 'unauthorized' signal
│   ├── hooks/
│   │   └── useRealtimeSync.ts  # bridges stickyWS events → React Query invalidations;
│   │                           # connect/disconnect driven by authStore.token
│   ├── store/
│   │   ├── authStore.ts        # zustand + persist (JWT, username, expiresAt)
│   │   └── uiStore.ts          # zustand + persist (dark-mode preference)
│   ├── types/
│   │   ├── api.ts              # DTOs mirroring server/internal/handler response shapes
│   │   └── sticky.ts           # StickyNoteDTO (matches server), StickyView (UI model),
│   │                           # TodoFilter, STICKY_COLORS, DEFAULT_STICKY_COLOR
│   ├── lib/
│   │   ├── color.ts            # color → contrast helpers shared by cards
│   │   ├── format.ts           # date / priority formatters
│   │   └── stickyCodec.ts      # StickyNoteDTO ↔ StickyView conversion;
│   │                           # hex ↔ {red,green,blue,alpha} JSON, filter ↔ JSON string
│   ├── views/
│   │   ├── LoginView.tsx       # POST /api/login → writes authStore
│   │   └── StickyBoard.tsx     # useQuery(qk.stickies(), api.listStickies) → grid of StickyCard
│   └── components/
│       ├── AppBar.tsx          # history entry + "new sticky" (useMutation(api.upsertSticky))
│       ├── StickyCard.tsx      # one sticky (title + filter-bound TodoList)
│       ├── TodoList.tsx
│       ├── TodoRow.tsx
│       ├── DraftTodoRow.tsx
│       ├── EditTodoSheet.tsx   # edit modal → PUT /api/todos/:id
│       ├── FilterEditor.tsx    # sticky filter form (status/tag/keyword/...)
│       ├── HistoryView.tsx     # /api/audit-logs list (also mounts via AppBar)
│       └── Modal.tsx
├── vite.config.ts              # base: '/app/', proxy for /api & /health (HTTP only, see caveats)
├── tailwind.config.ts
├── postcss.config.js
├── tsconfig.json / tsconfig.app.json / tsconfig.node.json
└── package.json
```

## API contract

`src/api/client.ts` is the single entry point to the backend. Responsibilities:

- Read the JWT from `useAuthStore.getState().token` on every call, inject `Authorization: Bearer <token>`.
- Default `Content-Type: application/json` when a body is present.
- Normalize errors: any non-2xx response becomes `throw new ApiError(status, message)` where `message` prefers the server's `{ "error": "..." }` body.
- On `401`, call `useAuthStore.getState().logout()` so the next render snaps back to `LoginView` — deep links therefore survive an expired token without a full-page redirect.

All paths are **origin-relative** (`/api/todos`, `/health`, `/api/login`, `/api/sticky-notes/:id`, `/api/ws` ...). In dev, Vite proxies `/api` and `/health` to `:8080`. In production, the same Go binary serves SPA + API on a single origin, so no CORS is needed.

### Sticky DTO ↔ view conversion

Server-side (`server/internal/model/models.go`) the three extensible fields on `StickyNote` — `frame` / `bg_color` / `filter` — are **opaque JSON strings**. The server only validates `json.Valid(...)` + `len ≤ 4096`, so adding a new property to the sticky filter does **not** need a backend change.

The Web UI speaks a friendlier shape (`StickyView`) with a hex color and a decoded `TodoFilter`. `src/lib/stickyCodec.ts` owns the translation in both directions:

- **Decode (`dtoToView`)**: falls back to `DEFAULT_STICKY_COLOR` / `defaultFilter` if JSON parsing fails — dirty data must never block rendering.
- **Encode (`viewToUpsertRequest`)**: always emits valid JSON (the server's `json.Valid` gate must pass) and pins `frame = "{}"` — window positions are a macOS-only concern (see `AGENTS.md §3.4`).

## Realtime sync (WebSocket)

- **Transport**: `src/api/ws.ts` exports a singleton `stickyWS`. It dials `ws(s)://<window.location.host>/api/ws`, sends `{type:'auth',token}` within 2s, then waits for `{type:'ready'}` before marking the connection usable.
- **Event types** (must match `server/internal/ws/event.go`): `todo.created` / `todo.updated` / `todo.deleted` / `sticky.upserted` / `sticky.deleted`. The hub does **not** filter the sender — the client that issued the REST write also receives the event, so mutations rely on the REST response (not the WS echo) to update cache.
- **Event → cache**: `src/hooks/useRealtimeSync.ts` maps event types to `queryKey` prefixes via `EVENT_INVALIDATE_MAP` and calls `queryClient.invalidateQueries({predicate: isKeyPrefix})`. `reconnected` → invalidate `['todos']` + `['tags']` + `['stickies']` (full refetch because the hub has no event replay buffer). `unauthorized` → `authStore.logout()`.
- **Mount once**: `App.tsx` calls `useRealtimeSync()` at the root. Mounting it anywhere else would register duplicate event listeners and double-invalidate every query.
- **Dev-mode caveat**: `vite.config.ts` proxies `/api` without `ws: true`, so `http-proxy-middleware` will **not** forward the WS upgrade. When running `npm run dev` on `:5173` with the backend on `:8080`, the WS handshake fails at the proxy. Two workarounds:
  1. Pass an explicit base URL when testing realtime features: `stickyWS.connect(token, 'http://127.0.0.1:8080')` — this bypasses Vite.
  2. Point the browser at the backend directly (`http://127.0.0.1:8080/app/`) after running `scripts/package-web.sh`, which serves the built SPA from the Go embed.

## SPA routing & deployment

- `vite.config.ts` pins `base: '/app/'`. Every emitted asset URL is therefore `/app/assets/...`, matching the Go mount.
- The Go embed handler (`server/internal/webui/webui.go`) returns `index.html` for every non-asset `/app/*` path (SPA fallback) and serves `/app/assets/*` with `Cache-Control: public, max-age=31536000, immutable`. Hashed filenames guarantee cache busting.
- Response headers include `Content-Security-Policy: default-src 'self'; style-src 'self' 'unsafe-inline'; ...` — tightening or loosening this (e.g. to add a third-party analytics origin) requires editing `webui.go`.

## Caveats

- **No offline mode / service worker.** A broken network surfaces as a React Query error state, and `stickyWS` keeps retrying via exponential backoff in the background.
- **No sticky data in `localStorage`.** Since the cloud-source refactor there is no offline cache for sticky notes — when the backend is unreachable, `StickyBoard` shows a red error state rather than stale data. (Intentional: the previous `zustand/persist` setup produced confusing two-state behavior where the offline copy would silently override the server's truth on reconnect.)
- **No user management UI.** The app is single-tenant; credentials come from the server's env vars.
- **Sticky filter is a free-form JSON string.** The server only validates `json.Valid` + `len ≤ 4096`; extending the filter shape with a new field does not require a backend change — just add the property to `types/sticky.ts#TodoFilter` and render it in `FilterEditor.tsx`. Make sure `stickyCodec.ts` round-trips cleanly before/after your change.
- **`frame` is always `"{}"`.** The Web UI doesn't model a window position (stickies are rendered as cards in a CSS grid). The field stays in the DTO only so the macOS client's JSON schema is preserved — don't remove or null it out.
- **Dark mode** is a global `<html class="dark">` toggle, not Tailwind's `dark:` variant on individual elements — see `App.tsx`'s effect for the exact logic.
