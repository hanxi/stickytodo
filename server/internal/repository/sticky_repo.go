package repository

import (
	"context"
	"errors"
	"fmt"

	"gorm.io/gorm"
	"gorm.io/gorm/clause"

	"github.com/hanxi/todo-server/internal/model"
)

// StickyRepo 封装 StickyNote 的数据访问。
//
// 设计要点：
//   - 主键 ID 是客户端生成的 UUID 字符串，Repo 不负责生成，仅做存取
//   - List 不做分页：便签总量预期 < 50，整表返回按 updated_at DESC 排序即可
//   - 采用 Upsert 语义（主键冲突即覆盖）替代分离的 Create/Update，配合客户端已有的 UUID
//     可以让"新建便签"和"更新便签"共用同一个 PUT 接口，大幅简化客户端同步逻辑
//
// 关于 Upsert 的 updated_at 刷新：
//
//	GORM v1.31 的 clause.OnConflict + DoUpdates(AssignmentColumns) 会把 updated_at
//	也纳入 SET 列表，但 VALUES 取自结构体字段本身——若调用方不主动填 UpdatedAt，
//	第二次 Upsert 就会用零值 0001-01-01 覆盖掉 updated_at，导致按 updated_at
//	排序的 List 结果异常（参见 gorm issue #5389 / #4759）。
//	因此 Service 层有责任在调用前显式设置 n.UpdatedAt = time.Now()；
//	Repo 不自行注入时间，保持行为纯净、方便测试 mock。
type StickyRepo struct {
	db *gorm.DB
}

// NewStickyRepo 构造 StickyRepo。db 不允许为 nil。
func NewStickyRepo(db *gorm.DB) (*StickyRepo, error) {
	if db == nil {
		return nil, errors.New("sticky-repo: db must not be nil")
	}
	return &StickyRepo{db: db}, nil
}

// List 返回所有未软删便签，按 updated_at DESC 排序。
// 不做分页：预期总量小，前端一次拉全即可。
// 同一毫秒内更新的记录按 id DESC 稳定排序，避免前端渲染时顺序跳动。
func (r *StickyRepo) List(ctx context.Context) ([]model.StickyNote, error) {
	var items []model.StickyNote
	err := r.db.WithContext(ctx).
		Model(&model.StickyNote{}).
		Order("updated_at DESC").
		Order("id DESC").
		Find(&items).Error
	if err != nil {
		return nil, fmt.Errorf("sticky-repo: list: %w", err)
	}
	return items, nil
}

// Get 单条查询。软删记录视为不存在，返回 ErrNotFound。
// 不做 id 合法性校验（那是 service 层的职责），Repo 只忠实映射"查不到 = ErrNotFound"。
func (r *StickyRepo) Get(ctx context.Context, id string) (*model.StickyNote, error) {
	var n model.StickyNote
	err := r.db.WithContext(ctx).Where("id = ?", id).First(&n).Error
	if err != nil {
		if errors.Is(err, gorm.ErrRecordNotFound) {
			return nil, ErrNotFound
		}
		return nil, fmt.Errorf("sticky-repo: get %s: %w", id, err)
	}
	return &n, nil
}

// Upsert 幂等写入：按主键 id 覆盖全部可变字段；若不存在则插入。
//
// 使用 ON CONFLICT ... DO UPDATE（GORM 的 clause.OnConflict）而不是先查后写，
// 避免"客户端同时打两次 PUT"时的竞态。DoUpdates 显式列出需要覆盖的字段：
//   - title / frame / bg_color / filter：业务可变字段
//   - updated_at：用调用方传入的值（service 层已设为 time.Now()）
//
// 故意不覆盖 created_at：首次 INSERT 时 GORM 会自动填当前时间，后续 UPDATE 保持不变。
//
// 前置条件：调用方必须保证 n != nil、n.ID 非空、n.UpdatedAt 已被赋值为当前时间。
func (r *StickyRepo) Upsert(ctx context.Context, n *model.StickyNote) error {
	if n == nil {
		return errors.New("sticky-repo: upsert: n must not be nil")
	}
	if n.ID == "" {
		return errors.New("sticky-repo: upsert: id must not be empty")
	}
	if n.UpdatedAt.IsZero() {
		// 契约保护：service 层忘记设时间戳时提前失败，而不是把零值写进 DB。
		return errors.New("sticky-repo: upsert: UpdatedAt must be set by caller")
	}

	err := r.db.WithContext(ctx).
		Clauses(clause.OnConflict{
			Columns: []clause.Column{{Name: "id"}},
			DoUpdates: clause.AssignmentColumns([]string{
				"title", "frame", "bg_color", "filter", "updated_at",
			}),
		}).
		Create(n).Error
	if err != nil {
		return fmt.Errorf("sticky-repo: upsert %s: %w", n.ID, err)
	}
	return nil
}

// Delete 软删。对不存在或已软删的记录返回 ErrNotFound。
// GORM 检测到 StickyNote 有 DeletedAt 字段，会自动转换为 UPDATE deleted_at=now() 的软删。
func (r *StickyRepo) Delete(ctx context.Context, id string) error {
	res := r.db.WithContext(ctx).
		Where("id = ?", id).
		Delete(&model.StickyNote{})
	if res.Error != nil {
		return fmt.Errorf("sticky-repo: delete %s: %w", id, res.Error)
	}
	if res.RowsAffected == 0 {
		return ErrNotFound
	}
	return nil
}
