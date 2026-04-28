package ws

import (
	"log"
	"net/http"
	"net/url"

	"github.com/gin-gonic/gin"
	"github.com/gorilla/websocket"

	"github.com/hanxi/todo-server/internal/service"
)

// Handler 返回挂载到 /api/ws 的 gin.HandlerFunc。
//
// 流程：
//  1. HTTP → WebSocket 升级（CheckOrigin 根据 corsOrigins 收窄）
//  2. 客户端必须在 2s 内发 {"type":"auth","token":"<jwt>"}
//  3. 服务端校验 token，通过则回 {"type":"ready",...}，注册到 Hub 并启动 pumps
//  4. 任何一步失败：以 close code 4401（或底层 400/500）断开连接
//
// 注意：WebSocket 握手阶段浏览器不支持自定义 header，所以不能复用 REST
// 层的 Authorization: Bearer 机制；这里显式走首帧 auth 协议，鉴权不走
// middleware.Auth。
func Handler(hub *Hub, authSvc *service.AuthService, corsOrigins []string, logger *log.Logger) gin.HandlerFunc {
	if logger == nil {
		logger = log.Default()
	}

	upgrader := websocket.Upgrader{
		ReadBufferSize:  4096,
		WriteBufferSize: 4096,
		CheckOrigin:     makeOriginChecker(corsOrigins),
	}

	return func(c *gin.Context) {
		conn, err := upgrader.Upgrade(c.Writer, c.Request, nil)
		if err != nil {
			// upgrader 已经写过 HTTP 响应（400），这里只记录
			logger.Printf("ws: upgrade failed: %v", err)
			return
		}

		client := NewClient(conn, hub, logger)

		// 首帧 auth：直接调用 AuthService.ParseToken
		// 签名：ParseToken(tokenStr string) (actor string, err error)
		actor, err := client.authenticate(authSvc.ParseToken)
		if err != nil {
			logger.Printf("ws: auth failed: %v", err)
			client.closeWithCode(CloseCodeUnauthorized, "unauthorized")
			return
		}
		_ = actor // 当前单账号场景下 actor 仅用于日志/扩展，不做额外授权判断

		// 回 ready 帧（同步写，此时 writePump 尚未启动）
		if err := client.sendReady(); err != nil {
			logger.Printf("ws: send ready failed: %v", err)
			_ = conn.Close()
			return
		}

		hub.Register(client)
		client.Start()
	}
}

// makeOriginChecker 根据 CORS 白名单构造 CheckOrigin 函数：
//   - 无 Origin header：非浏览器客户端（curl / Swift URLSession 等）直接放行
//   - corsOrigins 含 "*"：放行所有 Origin（开发环境）
//   - Origin 精确命中 corsOrigins 白名单：放行
//   - 同源请求（Origin 的 host[:port] 与请求的 Host 一致）：放行
//   - 其他：拒绝
//
// 与 router.corsMiddleware 的策略保持一致（按精确 allowlist 匹配），
// 但额外多一条"同源放行"——router 的 CORS 中间件依赖浏览器在同源时不发送 Origin，
// 而部分浏览器对 WebSocket 握手仍会带上 Origin（见 RFC 6455 §10.2），因此
// WS 侧必须显式放行同源，否则会把 /app/ 内嵌前端的 WS 连接拒之门外。
func makeOriginChecker(corsOrigins []string) func(r *http.Request) bool {
	allowAll := false
	allow := make(map[string]struct{}, len(corsOrigins))
	for _, o := range corsOrigins {
		if o == "*" {
			allowAll = true
			continue
		}
		allow[o] = struct{}{}
	}
	return func(r *http.Request) bool {
		origin := r.Header.Get("Origin")
		if origin == "" {
			return true
		}
		if allowAll {
			return true
		}
		if _, ok := allow[origin]; ok {
			return true
		}
		return isSameOrigin(r, origin)
	}
}

// isSameOrigin 判断 Origin header 是否与请求本身同源。
//
// "同源"的定义：Origin 的 host[:port] 与 r.Host 完全一致。
// 不比较 scheme——因为 r.Host 不带 scheme，且浏览器在 http↔ws / https↔wss
// 之间切换时 scheme 天然不同；只要 host:port 对齐即可认为是同一"源"的 WS 升级。
//
// Origin 解析失败或 Host 为空时视为不同源（拒绝），避免畸形 Origin 被误放行。
func isSameOrigin(r *http.Request, origin string) bool {
	if r.Host == "" {
		return false
	}
	u, err := url.Parse(origin)
	if err != nil || u.Host == "" {
		return false
	}
	return u.Host == r.Host
}
