package router

import (
	"errors"
	"io"
	"log"
	"net/http"
	"time"

	"github.com/gin-gonic/gin"

	"github.com/hanxi/todo-server/internal/handler"
	"github.com/hanxi/todo-server/internal/middleware"
	"github.com/hanxi/todo-server/internal/service"
	"github.com/hanxi/todo-server/internal/webui"
	"github.com/hanxi/todo-server/internal/ws"
)

// Deps 汇聚所有需要注入路由层的服务与 handler 依赖。
type Deps struct {
	Auth        *service.AuthService
	Todos       *service.TodoService
	Audit       *service.AuditService
	Stickies    *service.StickyService
	AuthH       *handler.AuthHandler
	TodoH       *handler.TodoHandler
	AuditH      *handler.AuditHandler
	TagH        *handler.TagHandler
	StickyH     *handler.StickyHandler
	WSHub       *ws.Hub     // WebSocket 连接中枢；/api/ws 路由依赖它完成广播
	CorsOrigins []string    // 允许的 CORS origin；空切片表示不注入 CORS 中间件
	Logger      *log.Logger // 用于 Gin access log；nil 则回退 gin.DefaultWriter
	Version     string      // 注入到 /health 响应里，便于运维排查；为空则返回 "unknown"
}

// Validate 校验 Deps 各字段均已初始化。
func (d *Deps) Validate() error {
	if d == nil {
		return errors.New("router: deps is nil")
	}
	if d.Auth == nil || d.Todos == nil || d.Audit == nil || d.Stickies == nil {
		return errors.New("router: services not initialized")
	}
	if d.AuthH == nil || d.TodoH == nil || d.AuditH == nil || d.TagH == nil || d.StickyH == nil {
		return errors.New("router: handlers not initialized")
	}
	if d.WSHub == nil {
		return errors.New("router: ws hub not initialized")
	}
	return nil
}

// Build 构造 Gin Engine 并注册所有路由。调用方决定 Gin 运行模式。
func Build(deps *Deps) (*gin.Engine, error) {
	if err := deps.Validate(); err != nil {
		return nil, err
	}
	r := gin.New()
	r.Use(gin.Recovery())
	// 统一 access log 输出到 main 提供的 logger，避免两套日志格式并存。
	var accessWriter io.Writer = gin.DefaultWriter
	if deps.Logger != nil {
		accessWriter = deps.Logger.Writer()
	}
	r.Use(gin.LoggerWithWriter(accessWriter))
	if len(deps.CorsOrigins) > 0 {
		r.Use(corsMiddleware(deps.CorsOrigins))
	}

	version := deps.Version
	if version == "" {
		version = "unknown"
	}

	// 公共接口
	r.GET("/health", func(c *gin.Context) {
		c.JSON(http.StatusOK, gin.H{
			"status":  "ok",
			"time":    time.Now().UTC().Format(time.RFC3339),
			"server":  "todo-server",
			"version": version,
		})
	})
	r.POST("/api/login", deps.AuthH.Login)

	// WebSocket 实时事件通道：故意**不**挂在 authed group 下。
	// 浏览器的 WebSocket API 无法在 upgrade 请求上附加 Authorization header，
	// 所以鉴权走"首帧 auth 协议"（见 ws/handler.go 与 ws/client.go.authenticate）。
	// CSP 中 connect-src 'self' 已经允许同源 WS 升级，无需额外调整。
	r.GET("/api/ws", ws.Handler(deps.WSHub, deps.Auth, deps.CorsOrigins, deps.Logger))

	// 鉴权接口
	authed := r.Group("/api")
	authed.Use(middleware.Auth(deps.Auth))
	{
		authed.GET("/todos", deps.TodoH.List)
		authed.POST("/todos", deps.TodoH.Create)
		authed.GET("/todos/:id", deps.TodoH.Get)
		authed.PUT("/todos/:id", deps.TodoH.Update)
		authed.DELETE("/todos/:id", deps.TodoH.Delete)
		authed.POST("/todos/:id/complete", deps.TodoH.Complete)
		authed.POST("/todos/:id/reopen", deps.TodoH.Reopen)
		authed.POST("/todos/:id/restore", deps.TodoH.Restore)
		authed.GET("/todos/:id/history", deps.AuditH.ListTodoHistory)

		authed.GET("/audit-logs", deps.AuditH.ListAuditLogs)
		authed.GET("/tags", deps.TagH.List)

		// 便签 API（跨端同步）：创建/更新共用 PUT（幂等），id 由客户端生成 UUID。
		authed.GET("/sticky-notes", deps.StickyH.List)
		authed.GET("/sticky-notes/:id", deps.StickyH.Get)
		authed.PUT("/sticky-notes/:id", deps.StickyH.Upsert)
		authed.DELETE("/sticky-notes/:id", deps.StickyH.Delete)
	}

	// Web UI：/app 下挂载 embed 进来的前端静态资源 + SPA fallback。
	// webui.Handler 内部已处理 CSP / SPA fallback / 未构建 placeholder 等细节。
	uiHandler, err := webui.Handler("/app")
	if err != nil {
		return nil, err
	}
	// gin.WrapH 把 http.Handler 适配为 gin.HandlerFunc；Any 让所有方法命中同一 handler，
	// handler 内部会 405 非 GET/HEAD。wildcard 路由 *filepath 要求必须包含至少一个字符，
	// 所以单独再注册 /app 的精确路径，重定向到 /app/ 让前端资源路径正确解析。
	//
	// 同时注册 GET 和 HEAD：HEAD 与 GET 的语义应一致（HTTP 规范要求）。如果只注册
	// GET，Gin 的 RedirectTrailingSlash 兜底会对 HEAD /app 返回 307 而不是 301，
	// 造成客户端（curl -I、各类健康探针）看到的状态码与 GET 不一致。
	redirectToAppSlash := func(c *gin.Context) {
		c.Redirect(http.StatusMovedPermanently, "/app/")
	}
	r.GET("/app", redirectToAppSlash)
	r.HEAD("/app", redirectToAppSlash)
	r.Any("/app/*filepath", gin.WrapH(uiHandler))

	// 404 统一返回 JSON
	r.NoRoute(func(c *gin.Context) {
		c.JSON(http.StatusNotFound, gin.H{"error": "not found: " + c.Request.URL.Path})
	})
	r.NoMethod(func(c *gin.Context) {
		c.JSON(http.StatusMethodNotAllowed, gin.H{"error": "method not allowed"})
	})

	return r, nil
}

// corsMiddleware 返回一个简洁的 CORS 中间件。origins 为精确匹配的 allowlist。
// 不支持通配符；若要放开所有，使用 ["*"]。
func corsMiddleware(origins []string) gin.HandlerFunc {
	allowAll := false
	allow := make(map[string]struct{}, len(origins))
	for _, o := range origins {
		if o == "*" {
			allowAll = true
			continue
		}
		allow[o] = struct{}{}
	}
	return func(c *gin.Context) {
		origin := c.GetHeader("Origin")
		if origin != "" {
			if allowAll {
				c.Header("Access-Control-Allow-Origin", "*")
			} else if _, ok := allow[origin]; ok {
				c.Header("Access-Control-Allow-Origin", origin)
				c.Header("Vary", "Origin")
			}
			c.Header("Access-Control-Allow-Methods", "GET,POST,PUT,DELETE,OPTIONS")
			c.Header("Access-Control-Allow-Headers", "Authorization,Content-Type")
			c.Header("Access-Control-Max-Age", "600")
		}
		if c.Request.Method == http.MethodOptions {
			c.AbortWithStatus(http.StatusNoContent)
			return
		}
		c.Next()
	}
}
