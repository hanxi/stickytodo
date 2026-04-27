// Package webui 负责把前端构建产物（client/web/dist 经 scripts/package-web.sh 同步到
// 本包的 dist/ 目录）通过 go:embed 打入最终二进制，并暴露一个 http.Handler 给 router
// 挂到 /app 路由下。
//
// 设计取舍：
//   - 采用 embed 而非运行时读 dist/：打包后单一二进制即可分发，避免"忘记带 dist"。
//   - SPA fallback：访问不存在的子路径（如 /app/sticky/123）时返回 index.html，让
//     React Router / 浏览器 History API 接管。
//   - 资源类请求（带扩展名的 /app/assets/... 等）若 FS 里找不到，走 404，避免把 JS/CSS
//     请求也 fallback 成 HTML——否则浏览器会报 "Expected JavaScript, got text/html"。
//   - 不依赖 gin：handler 以 http.Handler 暴露，方便 router 用 Any 方式挂载，也便于单测。
//
// 编译期检查：如果前端尚未构建（没有 dist/index.html，只有 .gitkeep），Handler 在第一次
// 请求时才会回退到 placeholder 页；这样 go build 始终能过，开发者用 go run 直跑也能
// 看到明确提示。
package webui

import (
	"embed"
	"errors"
	"fmt"
	"io/fs"
	"net/http"
	"net/url"
	"path"
	"strings"
)

// Embedded 是被打入二进制的前端产物。
// `all:` 前缀确保以 `.` / `_` 开头的文件（包括 .gitkeep）也被包含——
// 但我们只会用 dist/index.html 与 dist/assets/** 去响应请求。
//
//go:embed all:dist
var Embedded embed.FS

// distFS 返回 dist/ 的 fs.FS 视图（去掉 "dist/" 前缀）。
func distFS() (fs.FS, error) {
	return fs.Sub(Embedded, "dist")
}

// placeholderHTML 在 dist/index.html 缺失时返回给浏览器，指导开发者先构建前端。
// 保持纯文本风格，避免引入任何外部依赖；顺便满足 CSP default-src 'self'。
const placeholderHTML = `<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="UTF-8">
  <title>StickyTodo · Web (not built)</title>
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <style>
    body { font-family: -apple-system, Segoe UI, Helvetica, Arial, sans-serif; margin: 4em auto; max-width: 680px; color: #222; line-height: 1.55; padding: 0 1em; }
    code { background: #f4f4f5; padding: 1px 6px; border-radius: 4px; font-size: 0.95em; }
    pre { background: #0f172a; color: #e2e8f0; padding: 12px 16px; border-radius: 6px; overflow-x: auto; }
    h1 { margin-bottom: 0.2em; }
    small { color: #666; }
  </style>
</head>
<body>
  <h1>StickyTodo Web UI not built yet</h1>
  <p>The Go binary is running, but <code>server/internal/webui/dist/index.html</code> is missing.</p>
  <p>Build the web bundle first, then restart the server:</p>
  <pre>scripts/package-web.sh     # from repo root
# or equivalently:
cd client/web &amp;&amp; npm install &amp;&amp; npm run build
cp -R client/web/dist/. server/internal/webui/dist/</pre>
  <p><small>This placeholder is served by <code>server/internal/webui/webui.go</code> and never ships in a release artifact (release pipeline builds the web UI before <code>go build</code>).</small></p>
</body>
</html>
`

// Handler 返回挂到 /app 下的 http.Handler。
//
// prefix 是该 handler 在外层 router 上的挂载前缀，用于从 URL.Path 里剥离（例如
// "/app"）。传入 "" 表示不剥离。传入的 prefix 必须以 "/" 开头且不以 "/" 结尾
// （如 "/app"）；不合法时返回错误。
//
// 行为：
//   - GET /app/           → dist/index.html（若缺失则 placeholder）
//   - GET /app/assets/x   → dist/assets/x（FS 找不到 → 404）
//   - GET /app/foo/bar    → dist/index.html（SPA fallback，仅当路径不带文件扩展名或以 "/" 结尾）
//   - 非 GET/HEAD         → 405
func Handler(prefix string) (http.Handler, error) {
	if prefix != "" {
		if !strings.HasPrefix(prefix, "/") {
			return nil, errors.New("webui: prefix must start with '/'")
		}
		if strings.HasSuffix(prefix, "/") && prefix != "/" {
			return nil, errors.New("webui: prefix must not end with '/'")
		}
	}

	sub, err := distFS()
	if err != nil {
		return nil, fmt.Errorf("webui: build sub FS: %w", err)
	}

	// 预先探测 index.html 是否真实存在，供 fallback 判断。每次请求重复读 FS 也 OK，
	// 但这里只读一次、把结果缓存为 bool，避免每个请求都吃一次 Open 开销。
	_, indexErr := fs.Stat(sub, "index.html")
	indexOK := indexErr == nil

	fileServer := http.FileServer(http.FS(sub))

	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodGet && r.Method != http.MethodHead {
			w.Header().Set("Allow", "GET, HEAD")
			http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
			return
		}

		// 统一安全头。CSP 允许 'self'（同源）+ data:（字体/图标），禁止 inline JS。
		// 前端 index.html 由 Vite 生成，不含 inline script；自定义图标若用 data: URI 也可用。
		h := w.Header()
		h.Set("X-Content-Type-Options", "nosniff")
		h.Set("Referrer-Policy", "no-referrer")
		h.Set("Content-Security-Policy",
			"default-src 'self'; "+
				"script-src 'self'; "+
				"style-src 'self' 'unsafe-inline'; "+
				"img-src 'self' data:; "+
				"font-src 'self' data:; "+
				"connect-src 'self'; "+
				"frame-ancestors 'none'; "+
				"base-uri 'self'")

		// 剥离挂载前缀，得到 dist 内的相对路径。
		urlPath := r.URL.Path
		if prefix != "" {
			if !strings.HasPrefix(urlPath, prefix) {
				// 不该被路由到这里，但兜底 404。
				http.NotFound(w, r)
				return
			}
			urlPath = strings.TrimPrefix(urlPath, prefix)
		}
		if urlPath == "" {
			urlPath = "/"
		}

		// 根路径直接返回 index.html（或 placeholder）。
		if urlPath == "/" {
			serveIndex(w, r, sub, indexOK)
			return
		}

		// 绝对化并去掉首 "/"，得到 fs 子路径。
		cleaned := path.Clean(urlPath)
		fsPath := strings.TrimPrefix(cleaned, "/")
		if fsPath == "" || fsPath == "." {
			serveIndex(w, r, sub, indexOK)
			return
		}

		// 探测目标是否真实存在（文件或目录）；不存在时走 SPA fallback 或 404。
		if _, err := fs.Stat(sub, fsPath); err == nil {
			// 命中真实文件/目录。Vite 构建的 assets 文件名自带内容 hash
			// （例如 assets/index-DOUNWpAd.js），所以可以安全地做 1 年长缓存 +
			// immutable；发版时文件名会变，浏览器不会拿到过期的 chunk。
			// 其他直接命中的文件（例如 favicon.ico、robots.txt）不享用长缓存，
			// 用 FileServer 默认行为即可。
			if strings.HasPrefix(fsPath, "assets/") {
				w.Header().Set("Cache-Control", "public, max-age=31536000, immutable")
			}
			// 让 http.FileServer 接管（它会处理 MIME、Range 等）。
			// 克隆 r.URL 并改写 Path 为 fs 期望的形式（以 "/" 开头），避免修改原始 request。
			newURL := cloneURLWithPath(r.URL, "/"+fsPath)
			rr := r.Clone(r.Context())
			rr.URL = newURL
			fileServer.ServeHTTP(w, rr)
			return
		}

		// 未命中。带扩展名的请求（.js/.css/.png/...）一律 404，避免把 JS 请求 fallback 成 HTML。
		if hasFileExtension(fsPath) {
			http.NotFound(w, r)
			return
		}
		// 无扩展名的 "看起来像页面路由" 的请求 → SPA fallback。
		serveIndex(w, r, sub, indexOK)
	}), nil
}

// serveIndex 将 dist/index.html 写回响应；缺失时返回 placeholder 并打 503-ish 的
// 200（仍然用 200，避免健康检查或代理误判；文案里已经明确说明未构建）。
func serveIndex(w http.ResponseWriter, _ *http.Request, sub fs.FS, indexOK bool) {
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	// index.html 绝不缓存——每次发版后内部的 assets 文件名会变（Vite 做了 hash），
	// 但 index.html 自身的 URL 不变，如果缓存旧的会指向已被删掉的 chunk。
	w.Header().Set("Cache-Control", "no-store")

	if !indexOK {
		_, _ = w.Write([]byte(placeholderHTML))
		return
	}
	data, err := fs.ReadFile(sub, "index.html")
	if err != nil {
		_, _ = w.Write([]byte(placeholderHTML))
		return
	}
	_, _ = w.Write(data)
}

// hasFileExtension 判断 URL 路径的最后一段是否带扩展名。用于区分"资源请求"与"页面路由"。
// 返回 false 的例子：/settings、/sticky/123、/app
// 返回 true 的例子：/assets/index-abc.js、/favicon.ico、/logo.png
func hasFileExtension(p string) bool {
	base := path.Base(p)
	return strings.Contains(base, ".")
}

// cloneURLWithPath 克隆 src 并把 Path / RawPath 替换为 newPath。
// 不会修改原始 URL，避免在并发请求里的 data race。
func cloneURLWithPath(src *url.URL, newPath string) *url.URL {
	if src == nil {
		return &url.URL{Path: newPath}
	}
	cp := *src
	cp.Path = newPath
	cp.RawPath = "" // 让 net/http 重新 escape
	return &cp
}
