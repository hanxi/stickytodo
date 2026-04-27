package handler

import (
	"errors"
	"fmt"
	"net/http"
	"strconv"
	"time"

	"github.com/gin-gonic/gin"

	"github.com/hanxi/todo-server/internal/middleware"
	"github.com/hanxi/todo-server/internal/repository"
	"github.com/hanxi/todo-server/internal/service"
)

// TodoHandler 处理 TODO 的 CRUD + 完成/取消/删除/恢复。
type TodoHandler struct {
	svc *service.TodoService
}

// NewTodoHandler 构造 TodoHandler。svc 不允许为 nil。
func NewTodoHandler(svc *service.TodoService) (*TodoHandler, error) {
	if svc == nil {
		return nil, errors.New("todo-handler: svc is nil")
	}
	return &TodoHandler{svc: svc}, nil
}

// createRequest POST /api/todos 入参。
type createRequest struct {
	Title    string     `json:"title"`
	Content  string     `json:"content"`
	Priority int        `json:"priority"`
	Tag      string     `json:"tag"`
	DueAt    *time.Time `json:"due_at"`
}

// updateRequest PUT /api/todos/:id 入参。
// 所有字段均为指针，nil 表示"不修改"。
// DueAt 特殊语义：clear_due_at=true 时强制写 NULL（忽略 due_at 值）。
type updateRequest struct {
	Title      *string    `json:"title"`
	Content    *string    `json:"content"`
	Priority   *int       `json:"priority"`
	Tag        *string    `json:"tag"`
	DueAt      *time.Time `json:"due_at"`
	ClearDueAt bool       `json:"clear_due_at"`
}

// Create POST /api/todos
func (h *TodoHandler) Create(c *gin.Context) {
	var req createRequest
	if err := c.ShouldBindJSON(&req); err != nil {
		c.JSON(http.StatusBadRequest, gin.H{"error": "invalid request body: " + err.Error()})
		return
	}
	t, err := h.svc.Create(c.Request.Context(), service.CreateInput{
		Title:    req.Title,
		Content:  req.Content,
		Priority: req.Priority,
		Tag:      req.Tag,
		DueAt:    req.DueAt,
	}, actionCtx(c))
	if err != nil {
		writeServiceError(c, err)
		return
	}
	c.JSON(http.StatusCreated, t)
}

// List GET /api/todos
// Query: status, tag, keyword, due_before(RFC3339), page, page_size, include_deleted, only_deleted
func (h *TodoHandler) List(c *gin.Context) {
	page, err := parseIntQuery(c, "page", 1)
	if err != nil {
		return
	}
	pageSize, err := parseIntQuery(c, "page_size", 20)
	if err != nil {
		return
	}
	includeDeleted, err := parseBoolQuery(c, "include_deleted", false)
	if err != nil {
		return
	}
	onlyDeleted, err := parseBoolQuery(c, "only_deleted", false)
	if err != nil {
		return
	}
	in := service.ListInput{
		Status:         c.Query("status"),
		Tag:            c.Query("tag"),
		Keyword:        c.Query("keyword"),
		IncludeDeleted: includeDeleted,
		OnlyDeleted:    onlyDeleted,
		Page:           page,
		PageSize:       pageSize,
	}
	if v := c.Query("due_before"); v != "" {
		t, err := time.Parse(time.RFC3339, v)
		if err != nil {
			c.JSON(http.StatusBadRequest, gin.H{"error": "invalid due_before: expect RFC3339"})
			return
		}
		in.DueBefore = &t
	}
	res, err := h.svc.List(c.Request.Context(), in)
	if err != nil {
		writeServiceError(c, err)
		return
	}
	c.JSON(http.StatusOK, res)
}

// Get GET /api/todos/:id
// Query: include_deleted=1 可返回软删记录。
func (h *TodoHandler) Get(c *gin.Context) {
	id, ok := parsePathID(c)
	if !ok {
		return
	}
	includeDeleted, err := parseBoolQuery(c, "include_deleted", false)
	if err != nil {
		return
	}
	t, err := h.svc.Get(c.Request.Context(), id, includeDeleted)
	if err != nil {
		writeServiceError(c, err)
		return
	}
	c.JSON(http.StatusOK, t)
}

// Update PUT /api/todos/:id
func (h *TodoHandler) Update(c *gin.Context) {
	id, ok := parsePathID(c)
	if !ok {
		return
	}
	var req updateRequest
	if err := c.ShouldBindJSON(&req); err != nil {
		c.JSON(http.StatusBadRequest, gin.H{"error": "invalid request body: " + err.Error()})
		return
	}
	t, err := h.svc.Update(c.Request.Context(), id, service.UpdateInput{
		Title:      req.Title,
		Content:    req.Content,
		Priority:   req.Priority,
		Tag:        req.Tag,
		DueAt:      req.DueAt,
		ClearDueAt: req.ClearDueAt,
	}, actionCtx(c))
	if err != nil {
		writeServiceError(c, err)
		return
	}
	c.JSON(http.StatusOK, t)
}

// Complete POST /api/todos/:id/complete
func (h *TodoHandler) Complete(c *gin.Context) {
	id, ok := parsePathID(c)
	if !ok {
		return
	}
	t, err := h.svc.Complete(c.Request.Context(), id, actionCtx(c))
	if err != nil {
		writeServiceError(c, err)
		return
	}
	c.JSON(http.StatusOK, t)
}

// Reopen POST /api/todos/:id/reopen
func (h *TodoHandler) Reopen(c *gin.Context) {
	id, ok := parsePathID(c)
	if !ok {
		return
	}
	t, err := h.svc.Reopen(c.Request.Context(), id, actionCtx(c))
	if err != nil {
		writeServiceError(c, err)
		return
	}
	c.JSON(http.StatusOK, t)
}

// Delete DELETE /api/todos/:id
func (h *TodoHandler) Delete(c *gin.Context) {
	id, ok := parsePathID(c)
	if !ok {
		return
	}
	if err := h.svc.Delete(c.Request.Context(), id, actionCtx(c)); err != nil {
		writeServiceError(c, err)
		return
	}
	c.JSON(http.StatusOK, gin.H{"id": id, "deleted": true})
}

// Restore POST /api/todos/:id/restore
func (h *TodoHandler) Restore(c *gin.Context) {
	id, ok := parsePathID(c)
	if !ok {
		return
	}
	t, err := h.svc.Restore(c.Request.Context(), id, actionCtx(c))
	if err != nil {
		writeServiceError(c, err)
		return
	}
	c.JSON(http.StatusOK, t)
}

// ---- helpers ----

// parsePathID 从 URL path 中解析 :id，失败时自动写 400 并返回 ok=false。
func parsePathID(c *gin.Context) (uint, bool) {
	raw := c.Param("id")
	n, err := strconv.ParseUint(raw, 10, 64)
	if err != nil || n == 0 {
		c.JSON(http.StatusBadRequest, gin.H{"error": "invalid id: " + raw})
		return 0, false
	}
	return uint(n), true
}

// maxPathStringIDLen 是字符串型 path id（如 StickyNote 的 UUID）的长度上限。
// 与 service 层 maxStickyIDLen 对齐，独立定义避免 handler 包反向依赖 service 常量。
const maxPathStringIDLen = 64

// parsePathStringID 解析字符串型 :id（如 UUID），长度 1..maxPathStringIDLen。
//
// 语义层面的字符集合法性由下游 service 层负责（各资源允许的 id 字符集可能不同），
// 本 helper 只做 handler 层能立即判断的长度边界检查，减少下游无效调用。
// 失败时自动写 400 并返回 ok=false。
func parsePathStringID(c *gin.Context) (string, bool) {
	raw := c.Param("id")
	if raw == "" {
		c.JSON(http.StatusBadRequest, gin.H{"error": "invalid id: must not be empty"})
		return "", false
	}
	if len(raw) > maxPathStringIDLen {
		c.JSON(http.StatusBadRequest, gin.H{
			"error": fmt.Sprintf("invalid id: length must be <= %d chars", maxPathStringIDLen),
		})
		return "", false
	}
	return raw, true
}

// parseIntQuery 从 query string 读取整数。
// 未传（空字符串）返回 def；非法值写 400 响应并返回错误。
func parseIntQuery(c *gin.Context, key string, def int) (int, error) {
	s := c.Query(key)
	if s == "" {
		return def, nil
	}
	n, err := strconv.Atoi(s)
	if err != nil {
		c.JSON(http.StatusBadRequest, gin.H{"error": "invalid " + key + ": expect integer"})
		return 0, err
	}
	return n, nil
}

// parseBoolQuery 严格解析 query string 的布尔值。
// 未传返回 def；非法值写 400 响应并返回错误。
func parseBoolQuery(c *gin.Context, key string, def bool) (bool, error) {
	s := c.Query(key)
	if s == "" {
		return def, nil
	}
	switch s {
	case "1", "true", "True", "TRUE", "yes", "on":
		return true, nil
	case "0", "false", "False", "FALSE", "no", "off":
		return false, nil
	default:
		c.JSON(http.StatusBadRequest, gin.H{"error": "invalid " + key + ": expect 0/1/true/false"})
		return false, errInvalidBool
	}
}

// errInvalidBool 供 parseBoolQuery 返回，handler 只需判断 err!=nil 即可退出。
var errInvalidBool = errors.New("invalid boolean query param")

// actionCtx 从 gin.Context 提取 Actor / IP / UA 组成 service.ActionContext。
func actionCtx(c *gin.Context) service.ActionContext {
	return service.ActionContext{
		Actor:     middleware.Actor(c),
		IP:        c.ClientIP(),
		UserAgent: c.Request.UserAgent(),
	}
}

// writeServiceError 根据错误类型映射为合适的 HTTP 状态码 + JSON 响应。
func writeServiceError(c *gin.Context, err error) {
	switch {
	case errors.Is(err, repository.ErrNotFound):
		c.JSON(http.StatusNotFound, gin.H{"error": err.Error()})
	case errors.Is(err, service.ErrInvalidInput),
		errors.Is(err, repository.ErrInvalidField),
		errors.Is(err, repository.ErrNoFields):
		c.JSON(http.StatusBadRequest, gin.H{"error": err.Error()})
	default:
		c.JSON(http.StatusInternalServerError, gin.H{"error": err.Error()})
	}
}
