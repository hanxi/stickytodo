package handler

import (
	"errors"
	"net/http"

	"github.com/gin-gonic/gin"

	"github.com/hanxi/todo-server/internal/model"
	"github.com/hanxi/todo-server/internal/service"
)

// StickyHandler 处理便签（StickyNote）相关的 HTTP 请求。
//
// 路由（统一挂在 /api/sticky-notes 下，鉴权同 TODO）：
//   - GET    /api/sticky-notes        → List
//   - GET    /api/sticky-notes/:id    → Get
//   - PUT    /api/sticky-notes/:id    → Upsert（幂等，id 由客户端在路径里给出）
//   - DELETE /api/sticky-notes/:id    → Delete（软删）
//
// 故意不提供 POST：创建与更新共用同一 PUT 语义，配合客户端生成 UUID 可大幅简化同步逻辑。
type StickyHandler struct {
	svc *service.StickyService
}

// NewStickyHandler 构造 StickyHandler。svc 不允许为 nil。
func NewStickyHandler(svc *service.StickyService) (*StickyHandler, error) {
	if svc == nil {
		return nil, errors.New("sticky-handler: svc is nil")
	}
	return &StickyHandler{svc: svc}, nil
}

// upsertStickyRequest PUT /api/sticky-notes/:id 的请求体。
//
// frame / bg_color / filter 都是 JSON 字符串（不是嵌套对象），服务端对它们只做
// json.Valid + 长度校验，不解析内部字段。这样客户端可以自由扩展布局/样式/筛选字段，
// 而不需要后端同步迭代 DTO（详见 service.normalizeStickyJSON 与 plan 文档）。
//
// id 不在请求体里——由 URL path 提供，避免"body.id 与 path id 不一致"的歧义。
type upsertStickyRequest struct {
	Title   string `json:"title"`
	Frame   string `json:"frame"`
	BgColor string `json:"bg_color"`
	Filter  string `json:"filter"`
}

// List GET /api/sticky-notes
//
// 返回 {"items": [...]}，按 updated_at DESC 排序。
// 为保证 JSON 永远是数组而不是 null（便于 Web / macOS 客户端直接 forEach），
// 空结果会被显式替换为空切片。
func (h *StickyHandler) List(c *gin.Context) {
	items, err := h.svc.List(c.Request.Context())
	if err != nil {
		writeServiceError(c, err)
		return
	}
	if items == nil {
		items = []model.StickyNote{}
	}
	c.JSON(http.StatusOK, gin.H{"items": items})
}

// Get GET /api/sticky-notes/:id
func (h *StickyHandler) Get(c *gin.Context) {
	id, ok := parsePathStringID(c)
	if !ok {
		return
	}
	n, err := h.svc.Get(c.Request.Context(), id)
	if err != nil {
		writeServiceError(c, err)
		return
	}
	c.JSON(http.StatusOK, n)
}

// Upsert PUT /api/sticky-notes/:id
//
// 无论便签是否已存在，都用整块字段覆盖的方式写入。
// 若请求体解析失败返回 400；若 service 校验失败（title 过长、JSON 非法等）也返回 400；
// 其他错误由 writeServiceError 映射为 500。
func (h *StickyHandler) Upsert(c *gin.Context) {
	id, ok := parsePathStringID(c)
	if !ok {
		return
	}
	var req upsertStickyRequest
	if err := c.ShouldBindJSON(&req); err != nil {
		c.JSON(http.StatusBadRequest, gin.H{"error": "invalid request body: " + err.Error()})
		return
	}
	n, err := h.svc.Upsert(c.Request.Context(), service.UpsertStickyInput{
		ID:      id,
		Title:   req.Title,
		Frame:   req.Frame,
		BgColor: req.BgColor,
		Filter:  req.Filter,
	}, actionCtx(c))
	if err != nil {
		writeServiceError(c, err)
		return
	}
	c.JSON(http.StatusOK, n)
}

// Delete DELETE /api/sticky-notes/:id
// 软删成功返回 200 {"id":"...","deleted":true}；不存在返回 404。
func (h *StickyHandler) Delete(c *gin.Context) {
	id, ok := parsePathStringID(c)
	if !ok {
		return
	}
	if err := h.svc.Delete(c.Request.Context(), id, actionCtx(c)); err != nil {
		writeServiceError(c, err)
		return
	}
	c.JSON(http.StatusOK, gin.H{"id": id, "deleted": true})
}
