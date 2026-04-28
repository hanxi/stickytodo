package service

// EventBroadcaster 是 service 层对外广播变更事件的语义化接口。
//
// 为什么定义在 service 包而不是 ws 包：
//   - ws 包已经反向依赖 service（需要 AuthService 做首帧 token 校验）
//   - 若 service 再 import ws 就会循环依赖
//
// 为什么采用"语义化方法"而不是 `Broadcast(Event)`：
//   - 让 service 完全不感知具体事件结构（type 字符串、字段编码方式）
//   - ws 包（或未来任何事件通道实现）自己决定如何把语义方法映射为帧结构
//   - 避免 service 层构造 ws.Event 再传回 ws 包，造成结构耦合
//
// 装配：
//   - 生产：`*ws.Hub` 不直接实现该接口；由 `ws.NewBroadcaster(hub)` 返回
//     一个 adapter 把语义方法翻译成 ws.Event 并调 hub.Broadcast
//   - 测试：可传 nil —— TodoService / StickyService 构造器检测到 nil 时
//     会落回 nopBroadcaster，等价于"不广播"
//
// 参数约定：
//   - data 必须是可被 encoding/json 序列化的具体结构体（通常是 *model.Todo
//     / *model.StickyNote），实现方会对它做一次 json.Marshal
//   - id 必须是资源主键（uint for Todo / string for StickyNote）
type EventBroadcaster interface {
	BroadcastTodoCreated(todo any)
	BroadcastTodoUpdated(todo any)
	BroadcastTodoDeleted(id uint)
	BroadcastStickyUpserted(sticky any)
	BroadcastStickyDeleted(id string)
}

// nopBroadcaster 是 nil broadcaster 的兜底实现。
// 当 TodoService / StickyService 构造器拿到 nil broadcaster 时，
// 内部会替换为 nopBroadcaster，避免每次广播前都 `if b != nil` 判空。
type nopBroadcaster struct{}

func (nopBroadcaster) BroadcastTodoCreated(any)     {}
func (nopBroadcaster) BroadcastTodoUpdated(any)     {}
func (nopBroadcaster) BroadcastTodoDeleted(uint)    {}
func (nopBroadcaster) BroadcastStickyUpserted(any)  {}
func (nopBroadcaster) BroadcastStickyDeleted(string) {}

// resolveBroadcaster 统一的 nil 兜底入口。service 构造器使用。
func resolveBroadcaster(b EventBroadcaster) EventBroadcaster {
	if b == nil {
		return nopBroadcaster{}
	}
	return b
}
