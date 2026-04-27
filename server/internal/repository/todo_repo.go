package repository

import (
	"context"
	"errors"
	"fmt"
	"strings"
	"time"

	"gorm.io/gorm"

	"github.com/hanxi/todo-server/internal/model"
)

// ErrNotFound TODO 不存在（或已硬删）。
var ErrNotFound = errors.New("todo not found")

// TodoRepo 封装 Todo 的数据访问。
type TodoRepo struct {
	db *gorm.DB
}

// NewTodoRepo 构造 TodoRepo。db 不允许为 nil。
func NewTodoRepo(db *gorm.DB) (*TodoRepo, error) {
	if db == nil {
		return nil, errors.New("repo: db must not be nil")
	}
	return &TodoRepo{db: db}, nil
}

// ListOptions Todo 列表过滤参数。
type ListOptions struct {
	// Status "pending" / "done"；空字符串表示不过滤。
	Status string
	// Tag 精确匹配标签；空字符串表示不过滤。
	Tag string
	// Keyword 在 Title + Content 中做 LIKE 模糊匹配（不区分大小写）；空字符串表示不过滤。
	Keyword string
	// DueBefore 只返回 DueAt 早于此时间的记录；nil 表示不过滤。
	DueBefore *time.Time
	// IncludeDeleted 是否包含软删除记录（对应 GORM 的 Unscoped）。
	IncludeDeleted bool
	// OnlyDeleted 仅查询软删除记录（优先级高于 IncludeDeleted）。
	OnlyDeleted bool
	// Page 从 1 开始；<=0 视为 1。
	Page int
	// PageSize <=0 视为 20，>200 视为 200。
	PageSize int
}

// ListResult 分页结果。
type ListResult struct {
	Items    []model.Todo `json:"items"`
	Total    int64        `json:"total"`
	Page     int          `json:"page"`
	PageSize int          `json:"page_size"`
}

// List 查询 Todo 列表，按 (优先级 DESC, due_at ASC NULLS LAST, id DESC) 排序。
func (r *TodoRepo) List(ctx context.Context, opts ListOptions) (*ListResult, error) {
	if opts.Page <= 0 {
		opts.Page = 1
	}
	if opts.PageSize <= 0 {
		opts.PageSize = 20
	}
	if opts.PageSize > 200 {
		opts.PageSize = 200
	}

	q := r.baseQuery(ctx, opts.IncludeDeleted, opts.OnlyDeleted).
		Model(&model.Todo{})

	if opts.Status != "" {
		q = q.Where("status = ?", opts.Status)
	}
	if opts.Tag != "" {
		q = q.Where("tag = ?", opts.Tag)
	}
	if opts.Keyword != "" {
		pat := "%" + strings.ToLower(opts.Keyword) + "%"
		q = q.Where("LOWER(title) LIKE ? OR LOWER(content) LIKE ?", pat, pat)
	}
	if opts.DueBefore != nil {
		q = q.Where("due_at IS NOT NULL AND due_at < ?", *opts.DueBefore)
	}

	var total int64
	if err := q.Count(&total).Error; err != nil {
		return nil, fmt.Errorf("todo-repo: count: %w", err)
	}

	var items []model.Todo
	// 排序规则：
	//   1. 优先级高的在前（priority DESC）
	//   2. 有截止时间的在前、按时间升序（NULLS LAST via CASE WHEN）
	//   3. 同条件下按 id 降序（最新创建的在前）
	// 用显式 CASE WHEN 避免依赖 SQLite 对 "col IS NULL" 排序表达式的解析行为。
	if err := q.
		Order("priority DESC").
		Order("CASE WHEN due_at IS NULL THEN 1 ELSE 0 END ASC").
		Order("due_at ASC").
		Order("id DESC").
		Limit(opts.PageSize).
		Offset((opts.Page - 1) * opts.PageSize).
		Find(&items).Error; err != nil {
		return nil, fmt.Errorf("todo-repo: find: %w", err)
	}
	return &ListResult{
		Items:    items,
		Total:    total,
		Page:     opts.Page,
		PageSize: opts.PageSize,
	}, nil
}

// GetByID 获取一条 Todo。IncludeDeleted=true 时会返回软删记录。
func (r *TodoRepo) GetByID(ctx context.Context, id uint, includeDeleted bool) (*model.Todo, error) {
	q := r.baseQuery(ctx, includeDeleted, false)
	var t model.Todo
	if err := q.First(&t, "id = ?", id).Error; err != nil {
		if errors.Is(err, gorm.ErrRecordNotFound) {
			return nil, ErrNotFound
		}
		return nil, fmt.Errorf("todo-repo: get %d: %w", id, err)
	}
	return &t, nil
}

// Create 新建 Todo。status 为空时默认为 "pending"；id 将被忽略。
func (r *TodoRepo) Create(ctx context.Context, t *model.Todo) error {
	if t == nil {
		return errors.New("todo-repo: create: t must not be nil")
	}
	t.ID = 0
	if strings.TrimSpace(t.Status) == "" {
		t.Status = "pending"
	}
	if err := r.db.WithContext(ctx).Create(t).Error; err != nil {
		return fmt.Errorf("todo-repo: create: %w", err)
	}
	return nil
}

// ErrInvalidField 更新时传入的字段名不在白名单内。
var ErrInvalidField = errors.New("invalid field name for update")

// ErrNoFields 更新时未提供任何有效字段。
var ErrNoFields = errors.New("no fields to update")

// allowedUpdateFields 列出 Update 方法允许写入的字段集合。
// 显式白名单，防止外部意外传入 id/created_at 等只读字段。
var allowedUpdateFields = map[string]struct{}{
	"title":        {},
	"content":      {},
	"priority":     {},
	"tag":          {},
	"due_at":       {},
	"status":       {},
	"completed_at": {},
}

// Update 更新 Todo 的可变字段（Title/Content/Priority/Tag/DueAt/Status/CompletedAt）。
//
// 注意：
//   - 字段名必须是 allowedUpdateFields 中的 snake_case 列名，否则返回 ErrInvalidField
//   - fields 为空或去重后为空返回 ErrNoFields
//   - 若需把字段写为 NULL，请使用 gorm.Expr("NULL")（GORM 对 map 中的 nil 值会跳过）
//   - 返回更新后的最新记录
func (r *TodoRepo) Update(ctx context.Context, id uint, fields map[string]interface{}) (*model.Todo, error) {
	if len(fields) == 0 {
		return nil, ErrNoFields
	}
	// 严格校验：任意未知字段都报错，避免调用方拼写错误导致静默数据丢失。
	for k := range fields {
		if _, ok := allowedUpdateFields[k]; !ok {
			return nil, fmt.Errorf("%w: %q", ErrInvalidField, k)
		}
	}
	res := r.db.WithContext(ctx).
		Model(&model.Todo{}).
		Where("id = ?", id).
		Updates(fields)
	if res.Error != nil {
		return nil, fmt.Errorf("todo-repo: update %d: %w", id, res.Error)
	}
	if res.RowsAffected == 0 {
		return nil, ErrNotFound
	}
	return r.GetByID(ctx, id, false)
}

// Complete 原子性地将 Todo 标记为完成：status='done', completed_at=now。
// 若记录不存在或已软删，返回 ErrNotFound。
func (r *TodoRepo) Complete(ctx context.Context, id uint, now time.Time) (*model.Todo, error) {
	res := r.db.WithContext(ctx).
		Model(&model.Todo{}).
		Where("id = ?", id).
		Updates(map[string]interface{}{
			"status":       "done",
			"completed_at": now,
		})
	if res.Error != nil {
		return nil, fmt.Errorf("todo-repo: complete %d: %w", id, res.Error)
	}
	if res.RowsAffected == 0 {
		return nil, ErrNotFound
	}
	return r.GetByID(ctx, id, false)
}

// Reopen 原子性地把 Todo 改回未完成状态：status='pending', completed_at=NULL。
// 通过 gorm.Expr("NULL") 显式写 NULL（GORM 对 map 中的 nil 值会跳过而不是写 NULL）。
func (r *TodoRepo) Reopen(ctx context.Context, id uint) (*model.Todo, error) {
	res := r.db.WithContext(ctx).
		Model(&model.Todo{}).
		Where("id = ?", id).
		Updates(map[string]interface{}{
			"status":       "pending",
			"completed_at": gorm.Expr("NULL"),
		})
	if res.Error != nil {
		return nil, fmt.Errorf("todo-repo: reopen %d: %w", id, res.Error)
	}
	if res.RowsAffected == 0 {
		return nil, ErrNotFound
	}
	return r.GetByID(ctx, id, false)
}

// SoftDelete 软删除（GORM 会把 DeletedAt 置为当前时间）。
func (r *TodoRepo) SoftDelete(ctx context.Context, id uint) error {
	res := r.db.WithContext(ctx).Delete(&model.Todo{}, id)
	if res.Error != nil {
		return fmt.Errorf("todo-repo: soft delete %d: %w", id, res.Error)
	}
	if res.RowsAffected == 0 {
		return ErrNotFound
	}
	return nil
}

// Restore 恢复软删除记录。
func (r *TodoRepo) Restore(ctx context.Context, id uint) (*model.Todo, error) {
	// 用 Unscoped 才能定位到软删记录。
	res := r.db.WithContext(ctx).Unscoped().
		Model(&model.Todo{}).
		Where("id = ? AND deleted_at IS NOT NULL", id).
		Update("deleted_at", nil)
	if res.Error != nil {
		return nil, fmt.Errorf("todo-repo: restore %d: %w", id, res.Error)
	}
	if res.RowsAffected == 0 {
		return nil, ErrNotFound
	}
	return r.GetByID(ctx, id, false)
}

// ListTags 返回所有去重标签（非空）。
func (r *TodoRepo) ListTags(ctx context.Context) ([]string, error) {
	var tags []string
	err := r.db.WithContext(ctx).
		Model(&model.Todo{}).
		Where("tag IS NOT NULL AND tag <> ''").
		Distinct("tag").
		Order("tag ASC").
		Pluck("tag", &tags).Error
	if err != nil {
		return nil, fmt.Errorf("todo-repo: list tags: %w", err)
	}
	return tags, nil
}

// baseQuery 根据 includeDeleted / onlyDeleted 构造查询的起点。
func (r *TodoRepo) baseQuery(ctx context.Context, includeDeleted, onlyDeleted bool) *gorm.DB {
	db := r.db.WithContext(ctx)
	if onlyDeleted {
		return db.Unscoped().Where("deleted_at IS NOT NULL")
	}
	if includeDeleted {
		return db.Unscoped()
	}
	return db
}
