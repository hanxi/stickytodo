package ws

import (
	"encoding/json"
	"log"
	"sync"
)

// Hub 管理所有已鉴权的 WebSocket 客户端连接，提供非阻塞的事件广播能力。
//
// 生命周期：
//  1. main.go 构造 hub := NewHub(logger)
//  2. go hub.Run() 启动后台派发 goroutine
//  3. handler.go 在 auth 成功后调用 hub.Register(client)
//  4. service 层写操作成功后调用 hub.Broadcast(event)
//  5. 进程退出时 hub.Close() 关闭所有连接
//
// 并发模型：
//   - 所有外部 API（Register/Unregister/Broadcast/Close）都是线程安全的
//   - 内部用 clients map + RWMutex 维护连接集合；broadcast 只需 RLock
//   - 每个 Client 的 send channel 缓冲 256，溢出即判定慢客户端 → drop + close
type Hub struct {
	mu      sync.RWMutex
	clients map[*Client]struct{}
	logger  *log.Logger
	closed  bool
}

// NewHub 构造一个 Hub。logger 为 nil 时使用标准库默认 logger，方便单测。
func NewHub(logger *log.Logger) *Hub {
	if logger == nil {
		logger = log.Default()
	}
	return &Hub{
		clients: make(map[*Client]struct{}),
		logger:  logger,
	}
}

// Run 当前是一个空占位：本实现没有单点派发 goroutine，Broadcast 直接
// 扇出到各 Client.send channel（非阻塞），无需中心化循环。保留 Run 是为了
// main.go `go hub.Run()` 的调用语义统一，以及将来需要收敛 metrics /
// 周期性清理时的扩展点。
func (h *Hub) Run() {
	// no-op
}

// Register 登记一个已完成 auth 的客户端。若 Hub 已关闭则立即关闭该 client。
func (h *Hub) Register(c *Client) {
	h.mu.Lock()
	defer h.mu.Unlock()
	if h.closed {
		// Hub 已关闭，拒绝新连接
		c.close()
		return
	}
	h.clients[c] = struct{}{}
	h.logger.Printf("ws: client registered, total=%d", len(h.clients))
}

// Unregister 移除一个客户端（幂等）。通常由 Client 的 writePump/readPump 退出时调用。
func (h *Hub) Unregister(c *Client) {
	h.mu.Lock()
	defer h.mu.Unlock()
	if _, ok := h.clients[c]; ok {
		delete(h.clients, c)
		c.close()
		h.logger.Printf("ws: client unregistered, total=%d", len(h.clients))
	}
}

// Broadcast 把 event 扇出到所有客户端。
//
// 实现要点：
//   - 先 json.Marshal 一次，所有客户端共享同一个字节切片，避免重复序列化
//   - 对每个 client 的 send channel 做非阻塞 send（select + default）；
//     channel 满意味着该客户端消费不过来，直接标记为 slow 并异步关闭，
//     避免拖累整个广播
//   - 仅持有 RLock，与 Register/Unregister 互斥但并发广播不互相阻塞
func (h *Hub) Broadcast(event Event) {
	payload, err := json.Marshal(event)
	if err != nil {
		h.logger.Printf("ws: marshal event %q failed: %v", event.Type, err)
		return
	}

	h.mu.RLock()
	slow := make([]*Client, 0)
	for c := range h.clients {
		select {
		case c.send <- payload:
			// 正常投递
		default:
			// send channel 满，标记慢客户端
			slow = append(slow, c)
		}
	}
	h.mu.RUnlock()

	// 在 RLock 释放后再处理慢客户端，避免在广播链路里长时间持锁
	for _, c := range slow {
		h.logger.Printf("ws: client send buffer full, dropping connection")
		h.Unregister(c)
	}
}

// Close 关闭 Hub：标记状态、关闭所有 client，拒绝后续 Register。
// 幂等。
func (h *Hub) Close() {
	h.mu.Lock()
	defer h.mu.Unlock()
	if h.closed {
		return
	}
	h.closed = true
	for c := range h.clients {
		c.close()
		delete(h.clients, c)
	}
	h.logger.Printf("ws: hub closed")
}

// Len 返回当前注册客户端数量（主要用于测试与 metrics）。
func (h *Hub) Len() int {
	h.mu.RLock()
	defer h.mu.RUnlock()
	return len(h.clients)
}
