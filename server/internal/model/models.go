package model

import (
	"time"

	"gorm.io/gorm"
)

// Todo 代表一条待办事项，支持软删除以便历史回溯。
type Todo struct {
	ID          uint           `gorm:"primaryKey" json:"id"`
	Title       string         `gorm:"size:500;not null" json:"title"`
	Content     string         `gorm:"type:text" json:"content"`
	Status      string         `gorm:"size:20;index;not null;default:'pending'" json:"status"` // pending / done
	Priority    int            `gorm:"default:0" json:"priority"`                              // 0 最低 ~ 3 最高
	Tag         string         `gorm:"size:64;index" json:"tag"`
	DueAt       *time.Time     `gorm:"index" json:"due_at,omitempty"`
	CompletedAt *time.Time     `json:"completed_at,omitempty"`
	CreatedAt   time.Time      `json:"created_at"`
	UpdatedAt   time.Time      `json:"updated_at"`
	DeletedAt   gorm.DeletedAt `gorm:"index" json:"deleted_at,omitempty"`
}

// TableName 自定义表名。
func (Todo) TableName() string { return "todos" }

// AuditLog 记录所有写操作与登录事件，用于审计。
type AuditLog struct {
	ID        uint      `gorm:"primaryKey" json:"id"`
	TodoID    *uint     `gorm:"index" json:"todo_id,omitempty"`
	Action    string    `gorm:"size:32;index;not null" json:"action"` // create/update/complete/reopen/delete/restore/login/login_failed
	Detail    string    `gorm:"type:text" json:"detail"`              // JSON 字符串：变更前后字段或登录元信息
	Actor     string    `gorm:"size:64;index" json:"actor"`
	IP        string    `gorm:"size:64" json:"ip"`
	UserAgent string    `gorm:"size:256" json:"user_agent"`
	CreatedAt time.Time `gorm:"index" json:"created_at"`
}

// TableName 自定义表名。
func (AuditLog) TableName() string { return "audit_logs" }

// AppSecret 以 key/value 形式保存服务端启动期需要但又不适合写在环境变量的秘密值。
// 目前仅用于持久化 JWT 签名密钥（key="jwt_secret"），未来可扩展其他启动级密钥。
//
// 设计约束：
//   - Key 为主键，保证同一逻辑密钥在表里唯一，不可重复写入。
//   - Value 为 TEXT，不强制长度；具体长度由各 key 自行校验（jwt_secret = 64 char hex）。
//   - 不含软删（DeletedAt）：密钥一旦生成就应长期稳定，没有"软删"语义。
type AppSecret struct {
	Key       string    `gorm:"primaryKey;size:64" json:"key"`
	Value     string    `gorm:"type:text;not null" json:"value"`
	CreatedAt time.Time `json:"created_at"`
	UpdatedAt time.Time `json:"updated_at"`
}

// TableName 自定义表名。
func (AppSecret) TableName() string { return "app_secrets" }

// StickyNote 代表一个便签窗口的持久化状态。
//
// 与 Todo 不同，StickyNote 的主键 ID 由客户端生成（UUID 字符串），服务端不重新分配。
// 这样做的好处：
//   - macOS 客户端本地已使用 UUID 作为 StickyNote.id，保持一致可避免"上报前换 id"的同步复杂度
//   - Web 客户端新建便签时也在本地生成 UUID，乐观更新立即显示，随后 PUT 幂等落盘
//
// Frame / BgColor / Filter 均以 JSON 字符串整块存储：服务端完全不解析内部字段，
// 仅做长度 + json.Valid 粗粒度校验（见 service 层）。这样前端可以自由扩展样式字段，
// 后端 schema 不需要跟着迁移。
type StickyNote struct {
	ID        string         `gorm:"primaryKey;size:64" json:"id"`                    // 客户端 UUID，1..64 字符
	Title     string         `gorm:"size:200;not null;default:''" json:"title"`       // 便签标题
	Frame     string         `gorm:"type:text;not null;default:'{}'" json:"frame"`    // JSON: {x,y,width,height}
	BgColor   string         `gorm:"type:text;not null;default:'{}'" json:"bg_color"` // JSON: {red,green,blue,alpha}
	Filter    string         `gorm:"type:text;not null;default:'{}'" json:"filter"`   // JSON: TodoFilter
	CreatedAt time.Time      `json:"created_at"`
	UpdatedAt time.Time      `gorm:"index" json:"updated_at"`
	DeletedAt gorm.DeletedAt `gorm:"index" json:"deleted_at,omitempty"`
}

// TableName 自定义表名。
func (StickyNote) TableName() string { return "sticky_notes" }

// AllModels 列举全部需要 AutoMigrate 的模型。
func AllModels() []interface{} {
	return []interface{}{&Todo{}, &AuditLog{}, &AppSecret{}, &StickyNote{}}
}
