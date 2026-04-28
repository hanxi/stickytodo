// Package ws 实现 WebSocket 实时事件广播通道，用于把 REST 层的写操作
// 以事件帧的形式推送给所有已鉴权的浏览器 / macOS 客户端。
//
// 设计取舍：
//  1. 单账号：Hub 内只有一个全局广播域，不分 room / topic
//  2. 服务端单向推送业务事件；客户端除首帧 auth 外不上行业务消息
//  3. 出错策略：慢客户端（send channel 满）直接 drop + close，
//     不阻塞其他连接，也不做 message buffering（重连后客户端主动全量拉）
package ws

import (
	"encoding/json"
	"log"
)

// 事件类型常量。REST handler / service 层广播时直接使用这些字符串。
const (
	EventTodoCreated    = "todo.created"
	EventTodoUpdated    = "todo.updated"
	EventTodoDeleted    = "todo.deleted"
	EventStickyUpserted = "sticky.upserted"
	EventStickyDeleted  = "sticky.deleted"

	// EventReady 服务端在 auth 成功后回给客户端的第一帧。
	EventReady = "ready"
)

// Close code（4000-4999 为应用自定义区间）。
const (
	CloseCodeUnauthorized = 4401 // 未在超时内发送 auth，或 token 非法
	CloseCodeBadFrame     = 4400 // 客户端发了非法上行业务消息
)

// Event 是服务端推送给客户端的事件帧结构。
//
// 设计约定：
//   - 对资源变更事件（todo.*/sticky.*），Data 承载完整资源 JSON；删除类事件
//     使用 ID 字段传主键，避免把整块被删资源重传一遍
//   - ID 兼容 uint（Todo）与 string（StickyNote）两种主键类型，序列化时
//     omitempty 保证不会输出空值
type Event struct {
	Type string          `json:"type"`
	Data json.RawMessage `json:"data,omitempty"`
	ID   any             `json:"id,omitempty"`
}

// NewResourceEvent 构造一个 data 字段承载完整资源 JSON 的事件。
//
// payload 必须可被 encoding/json 序列化。序列化失败是不可恢复的内部错误
// （传入的资源通常是 GORM 模型，理应总可序列化）——此时返回 Data=null 的
// 降级事件，客户端收到后会触发 React Query invalidate 做全量拉取兜底。
//
// logger 不允许为 nil：序列化失败是罕见但必须可观测的异常，调用方必须
// 传入有效 logger（实践中由 ws.adapter 统一提供 Hub 的 logger）。
func NewResourceEvent(eventType string, payload any, logger *log.Logger) Event {
	if logger == nil {
		logger = log.Default()
	}
	raw, err := json.Marshal(payload)
	if err != nil {
		// 序列化失败：打日志 + 回退为 null data，让客户端全量拉
		logger.Printf("ws: marshal payload for event %q failed: %v", eventType, err)
		return Event{Type: eventType, Data: json.RawMessage("null")}
	}
	return Event{Type: eventType, Data: raw}
}

// NewDeleteEvent 构造一个只带资源主键的删除事件。
// id 类型为 any 以同时支持 uint（Todo）和 string（StickyNote）。
func NewDeleteEvent(eventType string, id any) Event {
	return Event{Type: eventType, ID: id}
}
