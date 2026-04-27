package middleware

import (
	"net/http"
	"strings"

	"github.com/gin-gonic/gin"

	"github.com/hanxi/todo-server/internal/service"
)

// Context key 常量，用于在 gin.Context 中携带认证后信息。
const (
	CtxKeyActor = "actor"
)

// Auth 返回一个 Gin 中间件，校验 Authorization: Bearer <jwt>。
// 成功时将 actor 放入 context（键为 CtxKeyActor），失败时以 401 终止。
func Auth(auth *service.AuthService) gin.HandlerFunc {
	if auth == nil {
		panic("middleware.Auth: auth service is nil")
	}
	return func(c *gin.Context) {
		tokenStr, err := extractBearerToken(c)
		if err != nil {
			c.AbortWithStatusJSON(http.StatusUnauthorized, gin.H{"error": err.Error()})
			return
		}
		actor, err := auth.ParseToken(tokenStr)
		if err != nil {
			c.AbortWithStatusJSON(http.StatusUnauthorized, gin.H{"error": "invalid token"})
			return
		}
		c.Set(CtxKeyActor, actor)
		c.Next()
	}
}

// Actor 从 Context 中取出已登录的 actor 用户名。
// 未登录或中间件未挂载时返回空串。
func Actor(c *gin.Context) string {
	v, ok := c.Get(CtxKeyActor)
	if !ok {
		return ""
	}
	s, _ := v.(string)
	return s
}

// extractBearerToken 从 Authorization 头中取出 Bearer token。
// 格式必须精确为 "Bearer <token>"（大小写不敏感），其他一律判非法。
func extractBearerToken(c *gin.Context) (string, error) {
	const prefix = "bearer "
	header := strings.TrimSpace(c.GetHeader("Authorization"))
	if header == "" {
		return "", errAuthHeaderMissing
	}
	if len(header) <= len(prefix) || !strings.EqualFold(header[:len(prefix)], prefix) {
		return "", errAuthHeaderInvalid
	}
	tok := strings.TrimSpace(header[len(prefix):])
	if tok == "" {
		return "", errAuthHeaderInvalid
	}
	return tok, nil
}

// 预定义错误，便于测试与比较。
var (
	errAuthHeaderMissing = &authErr{"authorization header missing"}
	errAuthHeaderInvalid = &authErr{"authorization header invalid"}
)

type authErr struct{ msg string }

func (e *authErr) Error() string { return e.msg }
