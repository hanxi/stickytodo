package service

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"regexp"
	"strings"
	"time"

	"github.com/hanxi/todo-server/internal/model"
	"github.com/hanxi/todo-server/internal/repository"
)

// Sticky 相关的字段长度上限。
const (
	// maxStickyIDLen 便签主键最大长度；标准 UUID v4 = 36 字符，预留到 64 以容纳带后缀的变体。
	maxStickyIDLen = 64

	// maxStickyTitleLen 便签标题最大长度（与 model.StickyNote.Title 的 size:200 对齐）。
	maxStickyTitleLen = 200

	// maxStickyJSONLen Frame / BgColor / Filter 单字段序列化后的最大字节数。
	// 粗估 4KB 足够容纳 TodoFilter（8 字段）+ Frame（4 float）+ RGBA；
	// 服务端不解析内部结构，只做长度 + json.Valid 校验。
	maxStickyJSONLen = 4 * 1024
)

// stickyIDPattern 宽松的便签 ID 格式校验：
//   - 允许：大小写字母、数字、短横线、下划线
//   - 长度由 maxStickyIDLen 单独限制（见 validateStickyID）
//
// 故意不强制 UUID v4 严格格式：
//   - macOS Swift 的 UUID().uuidString 是全大写 + 4 段短横线
//   - Web 端 crypto.randomUUID() 是全小写 + 4 段短横线
//   - 未来若要兼容短 id / 自定义前缀，当前字符集也能覆盖
//
// 此正则只拦截明显非法输入（空格、中文、控制字符、SQL 注入片段），
// 不做 UUID 语义校验——便签是单用户资源，ID 重复风险由客户端生成保证。
var stickyIDPattern = regexp.MustCompile(`^[A-Za-z0-9_-]+$`)

// StickyService 业务层：协调 StickyRepo 与 AuditService。
type StickyService struct {
	repo  *repository.StickyRepo
	audit *AuditService
	now   func() time.Time
}

// NewStickyService 构造 StickyService。repo 与 audit 均不允许为 nil。
func NewStickyService(repo *repository.StickyRepo, audit *AuditService) (*StickyService, error) {
	if repo == nil {
		return nil, errors.New("sticky-service: repo must not be nil")
	}
	if audit == nil {
		return nil, errors.New("sticky-service: audit must not be nil")
	}
	return &StickyService{repo: repo, audit: audit, now: time.Now}, nil
}

// List 返回所有便签（按 updated_at DESC 排序，见 Repo 层）。
func (s *StickyService) List(ctx context.Context) ([]model.StickyNote, error) {
	return s.repo.List(ctx)
}

// Get 单条查询。id 非法返回 ErrInvalidInput，找不到返回 repository.ErrNotFound。
func (s *StickyService) Get(ctx context.Context, id string) (*model.StickyNote, error) {
	if err := validateStickyID(id); err != nil {
		return nil, err
	}
	return s.repo.Get(ctx, id)
}

// UpsertStickyInput Upsert 入参。
//
// ID 必填（客户端生成 UUID）。Frame/BgColor/Filter 若为空字符串会被规范化为 "{}"
// 再落库，保证 DB 列永远是合法 JSON；非空时按原样透传。
type UpsertStickyInput struct {
	ID      string
	Title   string
	Frame   string
	BgColor string
	Filter  string
}

// Upsert 幂等写入便签。返回 DB 端最终记录（含服务端时间戳）。
//
// 时间戳处理：
//
//	服务端显式用 s.now() 同时设置 CreatedAt 和 UpdatedAt。
//	原因是 GORM v1.31 的 OnConflict + AssignmentColumns 不会自动刷新 updated_at，
//	必须在结构体里显式带上当前时间（SQLite 的 DO UPDATE 使用 excluded.updated_at，
//	而 excluded 来自 INSERT VALUES，即本结构体的 UpdatedAt 字段）。
//	CreatedAt 在"首次插入"场景由 GORM 写入 INSERT VALUES；在"已存在更新"场景，
//	created_at 不在 DoUpdates 列表中（见 repo 层 AssignmentColumns），DB 上
//	的值不会被本次调用的 now 覆盖。
func (s *StickyService) Upsert(ctx context.Context, in UpsertStickyInput, ac ActionContext) (*model.StickyNote, error) {
	if err := validateStickyID(in.ID); err != nil {
		return nil, err
	}

	title := strings.TrimSpace(in.Title)
	if len(title) > maxStickyTitleLen {
		return nil, fmt.Errorf("%w: title length must be <= %d", ErrInvalidInput, maxStickyTitleLen)
	}

	frame, err := normalizeStickyJSON("frame", in.Frame)
	if err != nil {
		return nil, err
	}
	bgColor, err := normalizeStickyJSON("bg_color", in.BgColor)
	if err != nil {
		return nil, err
	}
	filter, err := normalizeStickyJSON("filter", in.Filter)
	if err != nil {
		return nil, err
	}

	now := s.now()
	n := &model.StickyNote{
		ID:        in.ID,
		Title:     title,
		Frame:     frame,
		BgColor:   bgColor,
		Filter:    filter,
		CreatedAt: now, // INSERT 分支写入；UPDATE 分支不在 DoUpdates 中，不会覆盖
		UpdatedAt: now, // INSERT + UPDATE 都用；确保 OnConflict UPDATE 时刷新
	}
	if err := s.repo.Upsert(ctx, n); err != nil {
		return nil, err
	}

	// 写后重读：
	//   1. 已存在的记录 created_at 不该被本次覆盖，从 DB 读是唯一权威来源
	//   2. updated_at 可能因 DB 时区/精度与 Go 侧 now 有微小差异，以 DB 为准
	// 并发语义："最后写入者胜利"；SQLite 单写入者串行化保证不会读到部分写入。
	after, err := s.repo.Get(ctx, n.ID)
	if err != nil {
		return nil, err
	}
	// 审计 detail 只记录 after 完整快照，不记录 before/diff：
	//   - Upsert 语义是"整块状态覆盖"而非字段级 diff，before 对回溯价值有限
	//   - 省去 Upsert 前多一次"首次创建一定是 404"的 DB 查询
	//   - 想回溯历史变化可按 action=sticky_upsert + sticky_id 过滤 audit_logs
	//     拿到时间序列的 after 快照
	s.writeAudit(ctx, "sticky_upsert", ac, map[string]interface{}{
		"sticky_id": after.ID,
		"after":     stickySnapshot(after),
	})
	return after, nil
}

// Delete 软删便签。记录不存在时返回 repository.ErrNotFound。
//
// 为写审计会先读一次 before 做快照：
//   - ErrNotFound：before=nil，继续调用 repo.Delete 让其统一返回 ErrNotFound
//   - 其他 DB 错误：直接返回，不吞异常
func (s *StickyService) Delete(ctx context.Context, id string, ac ActionContext) error {
	if err := validateStickyID(id); err != nil {
		return err
	}
	before, err := s.repo.Get(ctx, id)
	if err != nil && !errors.Is(err, repository.ErrNotFound) {
		return err
	}
	if err := s.repo.Delete(ctx, id); err != nil {
		return err
	}
	detail := map[string]interface{}{"sticky_id": id}
	if before != nil {
		detail["before"] = stickySnapshot(before)
	}
	s.writeAudit(ctx, "sticky_delete", ac, detail)
	return nil
}

// writeAudit 统一审计入口。sticky 相关 action 的 TodoID 恒为 nil
// （便签不是 todo，不应复用 todo_id 索引语义）。审计失败只打日志，不影响主流程。
func (s *StickyService) writeAudit(ctx context.Context, action string, ac ActionContext, detail interface{}) {
	_ = s.audit.WriteAction(ctx, action, ac.Actor, ac.IP, ac.UserAgent, nil, detail)
}

// validateStickyID 校验便签主键：非空、长度 1..maxStickyIDLen、字符集 [A-Za-z0-9_-]。
func validateStickyID(id string) error {
	if id == "" {
		return fmt.Errorf("%w: id must not be empty", ErrInvalidInput)
	}
	if len(id) > maxStickyIDLen {
		return fmt.Errorf("%w: id length must be <= %d chars", ErrInvalidInput, maxStickyIDLen)
	}
	if !stickyIDPattern.MatchString(id) {
		return fmt.Errorf("%w: id must match pattern [A-Za-z0-9_-]+", ErrInvalidInput)
	}
	return nil
}

// normalizeStickyJSON 处理 frame/bg_color/filter 三个 JSON 字符串字段：
//   - 空字符串 → "{}"（DB 列永远持有合法 JSON）
//   - 非空字符串 → 按原样校验长度 + json.Valid，按原样落库（不做 trim）
//
// 不做 TrimSpace 是因为服务端对这三个字段承诺"透传"，客户端写什么、读回来就是什么；
// trim 会让调试多行缩进 JSON 的客户端看到字符串变化，干扰跨端同步排查。
// 只拦截明显坏数据，不校验内部 schema（前端可自由扩展字段，后端 schema 保持稳定）。
//
// field 仅用于错误消息，帮助客户端定位哪个字段出了问题。
func normalizeStickyJSON(field, raw string) (string, error) {
	if raw == "" {
		return "{}", nil
	}
	if len(raw) > maxStickyJSONLen {
		return "", fmt.Errorf("%w: %s length must be <= %d bytes", ErrInvalidInput, field, maxStickyJSONLen)
	}
	if !json.Valid([]byte(raw)) {
		return "", fmt.Errorf("%w: %s must be valid JSON", ErrInvalidInput, field)
	}
	return raw, nil
}

// stickySnapshot 审计详情用的快照。
//
// 把 frame/bg_color/filter 包装为 json.RawMessage，让 json.Marshal 审计 detail
// 时直接嵌入为嵌套 JSON 对象，而不是二次转义后的字符串（否则 audit_logs.detail
// 里会出现 "frame":"{\"x\":100,...}" 这种难读的内容）。
//
// 如果这三个字段在 DB 里意外存成了非合法 JSON（理论上被 normalizeStickyJSON 拦截），
// json.RawMessage 在 Marshal 时会直接失败，让审计层提前暴露脏数据。
func stickySnapshot(n *model.StickyNote) map[string]interface{} {
	if n == nil {
		return nil
	}
	return map[string]interface{}{
		"id":         n.ID,
		"title":      n.Title,
		"frame":      json.RawMessage(n.Frame),
		"bg_color":   json.RawMessage(n.BgColor),
		"filter":     json.RawMessage(n.Filter),
		"created_at": n.CreatedAt,
		"updated_at": n.UpdatedAt,
	}
}
