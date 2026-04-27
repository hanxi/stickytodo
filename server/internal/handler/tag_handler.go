package handler

import (
	"errors"
	"net/http"

	"github.com/gin-gonic/gin"

	"github.com/hanxi/todo-server/internal/service"
)

// TagHandler 处理标签相关请求。
type TagHandler struct {
	svc *service.TodoService
}

// NewTagHandler 构造 TagHandler。svc 不允许为 nil。
func NewTagHandler(svc *service.TodoService) (*TagHandler, error) {
	if svc == nil {
		return nil, errors.New("tag-handler: todo service is nil")
	}
	return &TagHandler{svc: svc}, nil
}

// List GET /api/tags
func (h *TagHandler) List(c *gin.Context) {
	tags, err := h.svc.ListTags(c.Request.Context())
	if err != nil {
		c.JSON(http.StatusInternalServerError, gin.H{"error": err.Error()})
		return
	}
	// 保证 JSON 里永远是数组而不是 null
	if tags == nil {
		tags = []string{}
	}
	c.JSON(http.StatusOK, gin.H{"tags": tags})
}
