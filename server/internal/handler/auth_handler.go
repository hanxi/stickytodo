package handler

import (
	"errors"
	"net/http"
	"time"

	"github.com/gin-gonic/gin"

	"github.com/hanxi/todo-server/internal/service"
)

// AuthHandler 处理登录相关 HTTP 请求。
type AuthHandler struct {
	auth  *service.AuthService
	audit *service.AuditService
}

// NewAuthHandler 构造 AuthHandler。auth/audit 均不允许为 nil。
func NewAuthHandler(auth *service.AuthService, audit *service.AuditService) (*AuthHandler, error) {
	if auth == nil {
		return nil, errors.New("auth-handler: auth service is nil")
	}
	if audit == nil {
		return nil, errors.New("auth-handler: audit service is nil")
	}
	return &AuthHandler{auth: auth, audit: audit}, nil
}

// loginRequest POST /api/login 入参。
type loginRequest struct {
	Username string `json:"username" binding:"required"`
	Password string `json:"password" binding:"required"`
}

// loginResponse POST /api/login 出参。
type loginResponse struct {
	Token     string    `json:"token"`
	ExpiresAt time.Time `json:"expires_at"`
	Username  string    `json:"username"`
}

// Login POST /api/login：校验用户名密码并返回 JWT。
func (h *AuthHandler) Login(c *gin.Context) {
	var req loginRequest
	if err := c.ShouldBindJSON(&req); err != nil {
		c.JSON(http.StatusBadRequest, gin.H{"error": "invalid request body: " + err.Error()})
		return
	}
	res, err := h.auth.Login(req.Username, req.Password)
	if err != nil {
		// 审计 login_failed。
		// 安全：不记录密码；reason 只记分类标签，不带底层错误消息，避免敏感信息落库。
		reason := "internal_error"
		if errors.Is(err, service.ErrInvalidCredentials) {
			reason = "bad_credentials"
		}
		_ = h.audit.WriteAction(c.Request.Context(), "login_failed",
			req.Username, c.ClientIP(), c.Request.UserAgent(), nil,
			map[string]interface{}{"reason": reason})
		if errors.Is(err, service.ErrInvalidCredentials) {
			c.JSON(http.StatusUnauthorized, gin.H{"error": "invalid username or password"})
			return
		}
		c.JSON(http.StatusInternalServerError, gin.H{"error": "login failed"})
		return
	}
	// 审计 login 成功
	_ = h.audit.WriteAction(c.Request.Context(), "login",
		req.Username, c.ClientIP(), c.Request.UserAgent(), nil, nil)
	c.JSON(http.StatusOK, loginResponse{
		Token:     res.Token,
		ExpiresAt: res.ExpiresAt,
		Username:  req.Username,
	})
}
