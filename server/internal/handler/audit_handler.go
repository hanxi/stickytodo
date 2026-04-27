package handler

import (
	"errors"
	"net/http"
	"time"

	"github.com/gin-gonic/gin"

	"github.com/hanxi/todo-server/internal/service"
)

// AuditHandler 处理审计日志相关 HTTP 请求。
// 依赖 todos 是为了在 ListTodoHistory 先校验 TODO 是否存在。
type AuditHandler struct {
	audit *service.AuditService
	todos *service.TodoService
}

// NewAuditHandler 构造 AuditHandler。audit/todos 均不允许为 nil。
func NewAuditHandler(audit *service.AuditService, todos *service.TodoService) (*AuditHandler, error) {
	if audit == nil {
		return nil, errors.New("audit-handler: audit service is nil")
	}
	if todos == nil {
		return nil, errors.New("audit-handler: todo service is nil")
	}
	return &AuditHandler{audit: audit, todos: todos}, nil
}

// ListTodoHistory GET /api/todos/:id/history
// 返回单条 TODO 的变更历史；包含软删记录的历史也可查看。
func (h *AuditHandler) ListTodoHistory(c *gin.Context) {
	id, ok := parsePathID(c)
	if !ok {
		return
	}
	// 先校验 TODO 是否存在（包含软删记录），避免客户端传不存在的 ID 却得到空数组。
	if _, err := h.todos.Get(c.Request.Context(), id, true); err != nil {
		writeServiceError(c, err)
		return
	}
	page, err := parseIntQuery(c, "page", 1)
	if err != nil {
		return
	}
	pageSize, err := parseIntQuery(c, "page_size", 50)
	if err != nil {
		return
	}
	res, err := h.audit.ListByTodo(c.Request.Context(), id, page, pageSize)
	if err != nil {
		c.JSON(http.StatusInternalServerError, gin.H{"error": err.Error()})
		return
	}
	c.JSON(http.StatusOK, res)
}

// ListAuditLogs GET /api/audit-logs
// Query: action, actor, todo_id, from(RFC3339), to(RFC3339), page, page_size
func (h *AuditHandler) ListAuditLogs(c *gin.Context) {
	page, err := parseIntQuery(c, "page", 1)
	if err != nil {
		return
	}
	pageSize, err := parseIntQuery(c, "page_size", 20)
	if err != nil {
		return
	}
	opts := service.ListOptions{
		Action:   c.Query("action"),
		Actor:    c.Query("actor"),
		Page:     page,
		PageSize: pageSize,
	}
	if v := c.Query("todo_id"); v != "" {
		n, perr := parseIntQuery(c, "todo_id", 0)
		if perr != nil {
			return
		}
		if n <= 0 {
			c.JSON(http.StatusBadRequest, gin.H{"error": "invalid todo_id: expect positive integer"})
			return
		}
		u := uint(n)
		opts.TodoID = &u
	}
	if v := c.Query("from"); v != "" {
		t, err := time.Parse(time.RFC3339, v)
		if err != nil {
			c.JSON(http.StatusBadRequest, gin.H{"error": "invalid from: expect RFC3339"})
			return
		}
		opts.From = t
	}
	if v := c.Query("to"); v != "" {
		t, err := time.Parse(time.RFC3339, v)
		if err != nil {
			c.JSON(http.StatusBadRequest, gin.H{"error": "invalid to: expect RFC3339"})
			return
		}
		opts.To = t
	}
	res, err := h.audit.List(c.Request.Context(), opts)
	if err != nil {
		c.JSON(http.StatusInternalServerError, gin.H{"error": err.Error()})
		return
	}
	c.JSON(http.StatusOK, res)
}
