package service

import (
	"context"
	"errors"
	"fmt"
	"strings"
	"time"

	"gorm.io/gorm"

	"github.com/hanxi/todo-server/internal/model"
	"github.com/hanxi/todo-server/internal/repository"
)

// ErrInvalidInput 业务校验失败（非 DB 错误）。用 errors.Is 可匹配。
var ErrInvalidInput = errors.New("invalid input")

// 合法状态集合。Todo.Status 只接受这两个值。
const (
	StatusPending = "pending"
	StatusDone    = "done"
)

// 优先级合法范围。
const (
	minPriority = 0
	maxPriority = 3
)

// 字段长度上限（字节）。防御性上限，避免恶意超大请求体。
const (
	maxTitleLen   = 500
	maxTagLen     = 64
	maxContentLen = 64 * 1024 // 64KB
)

// ActionContext 描述一次业务操作的调用者元信息，用于写审计日志。
// 全部字段都允许为空字符串（但调用方应尽量填写）。
type ActionContext struct {
	Actor     string // 操作者用户名（通常从 JWT 解析得到）
	IP        string // 客户端 IP
	UserAgent string // User-Agent
}

// TodoService 业务层：协调 TodoRepo 与 AuditService。
type TodoService struct {
	repo        *repository.TodoRepo
	audit       *AuditService
	broadcaster EventBroadcaster
	now         func() time.Time
}

// NewTodoService 构造 TodoService。repo 与 audit 均不允许为 nil。
// broadcaster 可为 nil：此时内部会替换为 nopBroadcaster，等价于"不广播"
// （用于单测 / 未启用 WS 的场景）。
func NewTodoService(repo *repository.TodoRepo, audit *AuditService, broadcaster EventBroadcaster) (*TodoService, error) {
	if repo == nil {
		return nil, errors.New("todo-service: repo must not be nil")
	}
	if audit == nil {
		return nil, errors.New("todo-service: audit must not be nil")
	}
	return &TodoService{
		repo:        repo,
		audit:       audit,
		broadcaster: resolveBroadcaster(broadcaster),
		now:         time.Now,
	}, nil
}

// CreateInput 新建 TODO 的入参。仅 Title 必填。
type CreateInput struct {
	Title    string     // 必填，去空白后不得为空，长度 <=500
	Content  string     // 可选，长度不限
	Priority int        // 0~3，默认 0
	Tag      string     // 可选，长度 <=64
	DueAt    *time.Time // 可选
}

// Create 新建 TODO。Status 固定为 pending。
func (s *TodoService) Create(ctx context.Context, in CreateInput, ac ActionContext) (*model.Todo, error) {
	title := strings.TrimSpace(in.Title)
	if title == "" {
		return nil, fmt.Errorf("%w: title must not be empty", ErrInvalidInput)
	}
	if len(title) > maxTitleLen {
		return nil, fmt.Errorf("%w: title length must be <= %d", ErrInvalidInput, maxTitleLen)
	}
	if len(in.Content) > maxContentLen {
		return nil, fmt.Errorf("%w: content length must be <= %d bytes", ErrInvalidInput, maxContentLen)
	}
	if in.Priority < minPriority || in.Priority > maxPriority {
		return nil, fmt.Errorf("%w: priority must be in [%d,%d]", ErrInvalidInput, minPriority, maxPriority)
	}
	tag := strings.TrimSpace(in.Tag)
	if len(tag) > maxTagLen {
		return nil, fmt.Errorf("%w: tag length must be <= %d", ErrInvalidInput, maxTagLen)
	}

	t := &model.Todo{
		Title:    title,
		Content:  in.Content,
		Priority: in.Priority,
		Tag:      tag,
		DueAt:    in.DueAt,
		Status:   StatusPending,
	}
	if err := s.repo.Create(ctx, t); err != nil {
		return nil, err
	}
	s.writeAudit(ctx, "create", ac, &t.ID, map[string]interface{}{
		"after": todoSnapshot(t),
	})
	// 成功后广播事件：让所有 WS 订阅方（Web / macOS 其他客户端）即时感知新增
	s.broadcaster.BroadcastTodoCreated(t)
	return t, nil
}

// UpdateInput 更新 TODO 的入参。所有字段都是指针，nil 表示"不修改该字段"。
// 特殊地，DueAt 有两种含义：
//   - nil              → 不修改
//   - 非 nil 但指向零值 → 期望"清空截止时间"（写 NULL）
// 为了区分这两种场景，引入 ClearDueAt 布尔开关：为 true 时将 due_at 写 NULL，
// 此时 DueAt 的值被忽略。
type UpdateInput struct {
	Title      *string
	Content    *string
	Priority   *int
	Tag        *string
	DueAt      *time.Time
	ClearDueAt bool
}

// HasAny 返回 UpdateInput 是否包含任何需要更新的字段。
func (u UpdateInput) HasAny() bool {
	return u.Title != nil || u.Content != nil || u.Priority != nil ||
		u.Tag != nil || u.DueAt != nil || u.ClearDueAt
}

// Update 更新 TODO 的可变字段，记录 before/after diff 到审计日志。
func (s *TodoService) Update(ctx context.Context, id uint, in UpdateInput, ac ActionContext) (*model.Todo, error) {
	if !in.HasAny() {
		return nil, fmt.Errorf("%w: no fields to update", ErrInvalidInput)
	}

	before, err := s.repo.GetByID(ctx, id, false)
	if err != nil {
		return nil, err
	}

	fields := make(map[string]interface{}, 6)
	changed := make(map[string]interface{}, 6)

	if in.Title != nil {
		title := strings.TrimSpace(*in.Title)
		if title == "" {
			return nil, fmt.Errorf("%w: title must not be empty", ErrInvalidInput)
		}
		if len(title) > maxTitleLen {
			return nil, fmt.Errorf("%w: title length must be <= %d", ErrInvalidInput, maxTitleLen)
		}
		if title != before.Title {
			fields["title"] = title
			changed["title"] = map[string]interface{}{"before": before.Title, "after": title}
		}
	}
	if in.Content != nil {
		if len(*in.Content) > maxContentLen {
			return nil, fmt.Errorf("%w: content length must be <= %d bytes", ErrInvalidInput, maxContentLen)
		}
		if *in.Content != before.Content {
			fields["content"] = *in.Content
			changed["content"] = map[string]interface{}{"before": before.Content, "after": *in.Content}
		}
	}
	if in.Priority != nil {
		if *in.Priority < minPriority || *in.Priority > maxPriority {
			return nil, fmt.Errorf("%w: priority must be in [%d,%d]", ErrInvalidInput, minPriority, maxPriority)
		}
		if *in.Priority != before.Priority {
			fields["priority"] = *in.Priority
			changed["priority"] = map[string]interface{}{"before": before.Priority, "after": *in.Priority}
		}
	}
	if in.Tag != nil {
		tag := strings.TrimSpace(*in.Tag)
		if len(tag) > maxTagLen {
			return nil, fmt.Errorf("%w: tag length must be <= %d", ErrInvalidInput, maxTagLen)
		}
		if tag != before.Tag {
			fields["tag"] = tag
			changed["tag"] = map[string]interface{}{"before": before.Tag, "after": tag}
		}
	}
	if in.ClearDueAt {
		if before.DueAt != nil {
			// GORM 对 map 中的 nil 值会跳过，需要用 sql 表达式显式写 NULL
			fields["due_at"] = gorm.Expr("NULL")
			changed["due_at"] = map[string]interface{}{"before": *before.DueAt, "after": nil}
		}
	} else if in.DueAt != nil {
		if before.DueAt == nil || !before.DueAt.Equal(*in.DueAt) {
			fields["due_at"] = *in.DueAt
			var beforeVal interface{}
			if before.DueAt != nil {
				beforeVal = *before.DueAt
			}
			changed["due_at"] = map[string]interface{}{"before": beforeVal, "after": *in.DueAt}
		}
	}

	// 所有字段实际都没变化：直接返回 before，不写审计、不触发 DB 更新。
	if len(fields) == 0 {
		return before, nil
	}

	after, err := s.repo.Update(ctx, id, fields)
	if err != nil {
		return nil, err
	}
	s.writeAudit(ctx, "update", ac, &id, map[string]interface{}{
		"changed": changed,
	})
	s.broadcaster.BroadcastTodoUpdated(after)
	return after, nil
}

// Complete 将 TODO 标记完成。若已完成则幂等（不会更新 completed_at）。
func (s *TodoService) Complete(ctx context.Context, id uint, ac ActionContext) (*model.Todo, error) {
	before, err := s.repo.GetByID(ctx, id, false)
	if err != nil {
		return nil, err
	}
	if before.Status == StatusDone {
		return before, nil
	}
	after, err := s.repo.Complete(ctx, id, s.now())
	if err != nil {
		return nil, err
	}
	s.writeAudit(ctx, "complete", ac, &id, map[string]interface{}{
		"before": map[string]interface{}{"status": before.Status, "completed_at": before.CompletedAt},
		"after":  map[string]interface{}{"status": after.Status, "completed_at": after.CompletedAt},
	})
	// Complete 语义上是一次状态变更，属于 updated 范畴，广播 todo.updated
	s.broadcaster.BroadcastTodoUpdated(after)
	return after, nil
}

// Reopen 将 TODO 改回未完成状态。若已经是 pending 则幂等。
func (s *TodoService) Reopen(ctx context.Context, id uint, ac ActionContext) (*model.Todo, error) {
	before, err := s.repo.GetByID(ctx, id, false)
	if err != nil {
		return nil, err
	}
	if before.Status == StatusPending {
		return before, nil
	}
	after, err := s.repo.Reopen(ctx, id)
	if err != nil {
		return nil, err
	}
	s.writeAudit(ctx, "reopen", ac, &id, map[string]interface{}{
		"before": map[string]interface{}{"status": before.Status, "completed_at": before.CompletedAt},
		"after":  map[string]interface{}{"status": after.Status, "completed_at": after.CompletedAt},
	})
	s.broadcaster.BroadcastTodoUpdated(after)
	return after, nil
}

// Delete 软删除。再次删除同一条（已软删）返回 ErrNotFound。
func (s *TodoService) Delete(ctx context.Context, id uint, ac ActionContext) error {
	before, err := s.repo.GetByID(ctx, id, false)
	if err != nil {
		return err
	}
	if err := s.repo.SoftDelete(ctx, id); err != nil {
		return err
	}
	s.writeAudit(ctx, "delete", ac, &id, map[string]interface{}{
		"before": todoSnapshot(before),
	})
	// 删除事件 payload 只携带主键，客户端据此从本地 cache 移除对应项
	s.broadcaster.BroadcastTodoDeleted(id)
	return nil
}

// Restore 从软删状态恢复。若未被软删则返回 ErrNotFound（因为 repo.Restore 只匹配 deleted_at IS NOT NULL）。
func (s *TodoService) Restore(ctx context.Context, id uint, ac ActionContext) (*model.Todo, error) {
	after, err := s.repo.Restore(ctx, id)
	if err != nil {
		return nil, err
	}
	s.writeAudit(ctx, "restore", ac, &id, map[string]interface{}{
		"after": todoSnapshot(after),
	})
	// Restore 按同步协议（implementation_plan.md）归入 todo.updated 事件：
	// 广播 updated 会让订阅方基于 queryKey invalidate 触发重新 list，
	// 此时被恢复的 todo 会重新出现在默认列表视图里。不用 created 语义是
	// 为了让客户端事件处理保持"恢复走同一条 updated 通道"的简洁性，
	// 同时严格对齐 REST → WS 事件的既定映射，避免客户端再分支处理。
	s.broadcaster.BroadcastTodoUpdated(after)
	return after, nil
}

// Get 获取一条 TODO。includeDeleted 控制是否包含软删记录。
func (s *TodoService) Get(ctx context.Context, id uint, includeDeleted bool) (*model.Todo, error) {
	return s.repo.GetByID(ctx, id, includeDeleted)
}

// ListInput 查询 TODO 列表的入参。
type ListInput struct {
	Status         string     // pending / done / "" (不过滤)
	Tag            string
	Keyword        string
	DueBefore      *time.Time
	IncludeDeleted bool
	OnlyDeleted    bool
	Page           int
	PageSize       int
}

// List 查询 TODO 列表。
func (s *TodoService) List(ctx context.Context, in ListInput) (*repository.ListResult, error) {
	// 校验 Status
	if in.Status != "" && in.Status != StatusPending && in.Status != StatusDone {
		return nil, fmt.Errorf("%w: status must be one of \"\", pending, done", ErrInvalidInput)
	}
	return s.repo.List(ctx, repository.ListOptions{
		Status:         in.Status,
		Tag:            in.Tag,
		Keyword:        in.Keyword,
		DueBefore:      in.DueBefore,
		IncludeDeleted: in.IncludeDeleted,
		OnlyDeleted:    in.OnlyDeleted,
		Page:           in.Page,
		PageSize:       in.PageSize,
	})
}

// ListTags 返回已使用的 tag 列表（去重排序）。
func (s *TodoService) ListTags(ctx context.Context) ([]string, error) {
	return s.repo.ListTags(ctx)
}

// writeAudit 统一的审计写入入口。失败只打日志，不抛出。
func (s *TodoService) writeAudit(ctx context.Context, action string, ac ActionContext, todoID *uint, detail interface{}) {
	_ = s.audit.WriteAction(ctx, action, ac.Actor, ac.IP, ac.UserAgent, todoID, detail)
}

// todoSnapshot 把 Todo 展开为稳定的 map，用于审计详情。
// 排除 DeletedAt（GORM 的类型），避免 JSON 输出不稳定。
func todoSnapshot(t *model.Todo) map[string]interface{} {
	if t == nil {
		return nil
	}
	m := map[string]interface{}{
		"id":       t.ID,
		"title":    t.Title,
		"content":  t.Content,
		"status":   t.Status,
		"priority": t.Priority,
		"tag":      t.Tag,
	}
	if t.DueAt != nil {
		m["due_at"] = *t.DueAt
	}
	if t.CompletedAt != nil {
		m["completed_at"] = *t.CompletedAt
	}
	return m
}
