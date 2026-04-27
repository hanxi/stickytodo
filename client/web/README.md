# stickytodo Web Client

React + Vite + Tailwind single-page app. Mirrors the macOS menu-bar client's multi-sticky-note UX in the browser, talking to the same Go backend REST API.

## Tech stack

- **React 18** + **TypeScript** (strict, `tsc -b --noEmit` as typecheck gate)
- **Vite 5** — dev server + production build. `base: '/app/'` so bundled asset URLs line up with the Go embed mount (`server/internal/webui/webui.go`).
- **Tailwind CSS** for styling; dark mode via `document.documentElement.classList.toggle('dark', …)` driven by the UI store.
- **Zustand** (`+ persist` middleware) for client-owned state that outlives a page reload:
  - `store/authStore.ts` — JWT / username / expiry, persisted to `localStorage` key `stickytodo.auth`
  - `store/stickyStore.ts` — sticky notes (id, title, color, filter), persisted to `stickytodo.stickies`
  - `store/uiStore.ts` — dark-mode preference, persisted to `stickytodo.ui`
- **@tanstack/react-query** for every server-owned resource (todos / tags / history / audit logs). Single `QueryClient` wired in `src/main.tsx` with `staleTime: 5s`, `retry: 1`, `refetchOnWindowFocus: false`. Query keys live in `src/api/queryKeys.ts` so invalidations stay DRY.
- Native `fetch` inside `src/api/client.ts` — no axios. Throws `ApiError { status, message }` for non-2xx responses; 401 auto-clears the auth store so the next render falls back to the login view.
- **date-fns** for formatting, **lucide-react** for icons, **clsx** for className composition.

## Local development

```bash
cd client/web
npm install
npm run dev        # http://127.0.0.1:5173
```

`vite.config.ts` proxies `/api/*` and `/health` to `http://127.0.0.1:8080`, so start the Go backend in a second terminal:

```bash
cd server && set -a && . ./.env && set +a && go run ./cmd/todo-server
```

Log in with `TODO_USERNAME` / `TODO_PASSWORD` from the server's `.env`.

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
│   ├── App.tsx                 # auth gate: token? StickyBoard : LoginView; also owns dark-mode effect
│   ├── index.css               # Tailwind entry
│   ├── api/
│   │   ├── client.ts           # fetch wrapper + ApiError + all REST calls
│   │   └── queryKeys.ts        # centralized react-query cache keys
│   ├── store/
│   │   ├── authStore.ts        # zustand + persist (JWT, username, expiresAt)
│   │   ├── stickyStore.ts      # zustand + persist (sticky notes array)
│   │   └── uiStore.ts          # zustand + persist (dark-mode preference)
│   ├── types/
│   │   ├── api.ts              # DTOs mirroring server/internal/handler response shapes
│   │   └── sticky.ts           # client-only sticky + filter types, DEFAULT_STICKY_COLOR
│   ├── lib/
│   │   ├── color.ts            # color → contrast helpers shared by cards
│   │   └── format.ts           # date / priority formatters
│   ├── views/
│   │   ├── LoginView.tsx       # POST /api/login → writes authStore
│   │   └── StickyBoard.tsx     # grid of StickyCard; default view after login
│   └── components/
│       ├── AppBar.tsx
│       ├── StickyCard.tsx      # one sticky (title + filter-bound TodoList)
│       ├── TodoList.tsx
│       ├── TodoRow.tsx
│       ├── DraftTodoRow.tsx
│       ├── EditTodoSheet.tsx   # edit modal → PUT /api/todos/:id
│       ├── FilterEditor.tsx    # sticky filter form (status/tag/keyword/...)
│       ├── HistoryView.tsx     # /api/audit-logs list (also mounts via AppBar)
│       └── Modal.tsx
├── vite.config.ts              # base: '/app/', proxy for /api & /health
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

All paths are **origin-relative** (`/api/todos`, `/health`, `/api/login` ...). In dev, Vite proxies them to `:8080`. In production, the same Go binary serves SPA + API on a single origin, so no CORS is needed.

## SPA routing & deployment

- `vite.config.ts` pins `base: '/app/'`. Every emitted asset URL is therefore `/app/assets/...`, matching the Go mount.
- The Go embed handler (`server/internal/webui/webui.go`) returns `index.html` for every non-asset `/app/*` path (SPA fallback) and serves `/app/assets/*` with `Cache-Control: public, max-age=31536000, immutable`. Hashed filenames guarantee cache busting.
- Response headers include `Content-Security-Policy: default-src 'self'; style-src 'self' 'unsafe-inline'; ...` — tightening or loosening this (e.g. to add a third-party analytics origin) requires editing `webui.go`.

## Caveats

- **No offline mode / service worker.** A broken network surfaces as a React Query error state.
- **No user management UI.** The app is single-tenant; credentials come from the server's env vars.
- **Sticky filter is a free-form JSON string.** The server only validates `json.Valid` + `len ≤ 4096`; extending the filter shape with a new field does not require a backend change — just add the property to `types/sticky.ts#TodoFilter` and render it in `FilterEditor.tsx`.
- **Dark mode** is a global `<html class="dark">` toggle, not Tailwind's `dark:` variant on individual elements — see `App.tsx`'s effect for the exact logic.
