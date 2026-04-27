package service

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"log"
	"time"

	"gorm.io/gorm"

	"github.com/hanxi/todo-server/internal/model"
)

// AuditService 负责写入与查询审计日志。
// 写入采用"尽力而为"策略：数据库错误不应中断业务操作，但必须通过 logger 打印错误。
type AuditService struct {
	db     *gorm.DB
	logger *log.Logger
}

// NewAuditService 构造 AuditService。db 不允许为 nil；logger 为 nil 时使用标准 log。
func NewAuditService(db *gorm.DB, logger *log.Logger) (*AuditService, error) {
	if db == nil {
		return nil, errors.New("audit: db must not be nil")
	}
	if logger == nil {
		logger = log.Default()
	}
	return &AuditService{db: db, logger: logger}, nil
}

// Write 写入一条审计日志。禁止传 nil。CreatedAt 由 GORM 自动填充。
// 任何失败（入参非法或 DB 错误）都会通过 logger 打印，并以 error 形式返回给调用方。
// 调用方一般可以忽略返回的 error——审计失败不应中断业务主流程；error 返回值保留主要用于测试验证。
func (s *AuditService) Write(ctx context.Context, entry *model.AuditLog) error {
	if entry == nil {
		err := errors.New("audit: entry must not be nil")
		s.logger.Printf("[audit] %v", err)
		return err
	}
	if entry.Action == "" {
		err := errors.New("audit: action must not be empty")
		s.logger.Printf("[audit] %v (actor=%q)", err, entry.Actor)
		return err
	}
	if err := s.db.WithContext(ctx).Create(entry).Error; err != nil {
		s.logger.Printf("[audit] write failed action=%s actor=%s err=%v",
			entry.Action, entry.Actor, err)
		return fmt.Errorf("audit: write: %w", err)
	}
	return nil
}

// WriteAction 便捷方法：构造一条日志并写入。detail 若非 nil 会被 JSON 序列化。
func (s *AuditService) WriteAction(ctx context.Context, action, actor, ip, ua string, todoID *uint, detail interface{}) error {
	entry := &model.AuditLog{
		Action:    action,
		Actor:     actor,
		IP:        ip,
		UserAgent: ua,
		TodoID:    todoID,
	}
	if detail != nil {
		b, err := json.Marshal(detail)
		if err != nil {
			s.logger.Printf("[audit] marshal detail failed action=%s actor=%s err=%v", action, actor, err)
			return fmt.Errorf("audit: marshal detail: %w", err)
		}
		entry.Detail = string(b)
	}
	return s.Write(ctx, entry)
}

// ListOptions 审计日志列表过滤与分页参数。
type ListOptions struct {
	// TodoID 仅查询与指定 TodoID 关联的日志；nil 表示不过滤。
	TodoID *uint
	// Action 仅查询指定 Action；空字符串表示不过滤。
	Action string
	// Actor 仅查询指定 Actor；空字符串表示不过滤。
	Actor string
	// From 只查询 created_at >= From 的日志；零值表示不过滤。
	From time.Time
	// To 只查询 created_at < To 的日志（左闭右开）；零值表示不过滤。
	To time.Time
	// Page 页码，从 1 开始；<=0 视为 1。
	Page int
	// PageSize 每页条数，<=0 视为 20，>200 视为 200。
	PageSize int
}

// ListResult 分页列表结果。
type ListResult struct {
	Items    []model.AuditLog `json:"items"`
	Total    int64            `json:"total"`
	Page     int              `json:"page"`
	PageSize int              `json:"page_size"`
}

// List 查询审计日志，按 created_at DESC 返回分页结果。
func (s *AuditService) List(ctx context.Context, opts ListOptions) (*ListResult, error) {
	if opts.Page <= 0 {
		opts.Page = 1
	}
	if opts.PageSize <= 0 {
		opts.PageSize = 20
	}
	if opts.PageSize > 200 {
		opts.PageSize = 200
	}
	q := s.db.WithContext(ctx).Model(&model.AuditLog{})
	if opts.TodoID != nil {
		q = q.Where("todo_id = ?", *opts.TodoID)
	}
	if opts.Action != "" {
		q = q.Where("action = ?", opts.Action)
	}
	if opts.Actor != "" {
		q = q.Where("actor = ?", opts.Actor)
	}
	if !opts.From.IsZero() {
		q = q.Where("created_at >= ?", opts.From)
	}
	if !opts.To.IsZero() {
		q = q.Where("created_at < ?", opts.To)
	}

	var total int64
	if err := q.Count(&total).Error; err != nil {
		return nil, fmt.Errorf("audit: count: %w", err)
	}

	var items []model.AuditLog
	if err := q.Order("created_at DESC, id DESC").
		Limit(opts.PageSize).
		Offset((opts.Page - 1) * opts.PageSize).
		Find(&items).Error; err != nil {
		return nil, fmt.Errorf("audit: find: %w", err)
	}

	return &ListResult{
		Items:    items,
		Total:    total,
		Page:     opts.Page,
		PageSize: opts.PageSize,
	}, nil
}

// ListByTodo 查询指定 TODO 的变更历史，按时间倒序。
// 登录事件（login / login_failed）本身 todo_id 为 NULL，不会落入此处结果。
func (s *AuditService) ListByTodo(ctx context.Context, todoID uint, page, pageSize int) (*ListResult, error) {
	tid := todoID
	return s.List(ctx, ListOptions{
		TodoID:   &tid,
		Page:     page,
		PageSize: pageSize,
	})
}
