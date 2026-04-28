package ws

import (
	"log"

	"github.com/hanxi/todo-server/internal/service"
)

// HubBroadcaster 把 *Hub 适配为 service.EventBroadcaster 接口。
//
// 为什么需要 adapter：
//   - service 包定义的 EventBroadcaster 是"语义化方法"（BroadcastTodoCreated
//     / BroadcastStickyDeleted 等），不暴露事件帧结构
//   - Hub.Broadcast 接收的是具体的 Event 结构（type + data + id）
//   - adapter 负责把语义方法的调用翻译成 Event 构造 + hub.Broadcast
//
// 为什么 adapter 放在 ws 包而不是 service 包：
//   - 构造 Event 需要知道 ws.EventTodoCreated 这些常量，天然属于 ws 包
//   - service 包仍保持对 ws 的零依赖，满足"避免循环依赖"的初衷
//
// 装配位置：main.go 调用 NewHubBroadcaster(hub) 拿到 service.EventBroadcaster，
// 再分别传给 NewTodoService / NewStickyService。
type HubBroadcaster struct {
	hub    *Hub
	logger *log.Logger
}

// NewHubBroadcaster 构造一个适配器。hub 不允许为 nil；logger 为 nil 时
// 回退到 log.Default（主要用于 marshal 失败日志，在极端情况下才会用到）。
//
// 返回类型是具体 *HubBroadcaster（实现了 service.EventBroadcaster），
// 调用方按需声明为 service.EventBroadcaster 即可。
func NewHubBroadcaster(hub *Hub, logger *log.Logger) *HubBroadcaster {
	if hub == nil {
		// adapter 对 hub 强依赖：hub 为 nil 时广播无意义。
		// 选择 panic 而不是返回错误，因为这在进程装配路径上属于编程错误，
		// 早失败比运行时偶发 nil 解引用更安全。
		panic("ws: NewHubBroadcaster: hub must not be nil")
	}
	if logger == nil {
		logger = log.Default()
	}
	return &HubBroadcaster{hub: hub, logger: logger}
}

// 编译期保证实现完备：任何一个方法漏加都会在此行报错。
var _ service.EventBroadcaster = (*HubBroadcaster)(nil)

// BroadcastTodoCreated 广播新建 Todo 事件，payload 为完整 Todo JSON。
func (b *HubBroadcaster) BroadcastTodoCreated(todo any) {
	b.hub.Broadcast(NewResourceEvent(EventTodoCreated, todo, b.logger))
}

// BroadcastTodoUpdated 广播 Todo 更新事件（含 complete / reopen / restore
// 这三个语义变更）。订阅方收到后应当根据 data.id 定位并替换本地缓存。
func (b *HubBroadcaster) BroadcastTodoUpdated(todo any) {
	b.hub.Broadcast(NewResourceEvent(EventTodoUpdated, todo, b.logger))
}

// BroadcastTodoDeleted 广播 Todo 软删事件，payload 仅含主键 id。
// 订阅方据此从本地 cache 移除对应项；因是软删，未来 Restore 会再广播
// todo.updated 把它"重新带回"。
func (b *HubBroadcaster) BroadcastTodoDeleted(id uint) {
	b.hub.Broadcast(NewDeleteEvent(EventTodoDeleted, id))
}

// BroadcastStickyUpserted 广播便签新增/更新事件。
// 因为 StickyNote 是 PUT 幂等 upsert，客户端不需要区分"新建"和"更新"——
// 同一个事件 type 覆盖两种语义，订阅方按 id 做 upsert 即可。
func (b *HubBroadcaster) BroadcastStickyUpserted(sticky any) {
	b.hub.Broadcast(NewResourceEvent(EventStickyUpserted, sticky, b.logger))
}

// BroadcastStickyDeleted 广播便签删除事件，payload 仅含 string 主键。
func (b *HubBroadcaster) BroadcastStickyDeleted(id string) {
	b.hub.Broadcast(NewDeleteEvent(EventStickyDeleted, id))
}
