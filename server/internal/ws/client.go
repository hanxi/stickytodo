package ws

import (
	"encoding/json"
	"errors"
	"log"
	"sync"
	"time"

	"github.com/gorilla/websocket"
)

// Sentinel errors for auth-frame-level protocol violations.
//
// 不复用 websocket.ErrBadHandshake：那是 gorilla/websocket 库预留给 HTTP
// upgrade 握手失败的 error，语义是"HTTP → WS 升级阶段出错"。本包的 auth
// 发生在 WS 连接建立之后的首帧消息校验，属于应用协议层错误，用独立 sentinel
// 便于排查日志、也便于未来做 errors.Is 分支判断。
var (
	errAuthFrameMalformed = errors.New("ws: auth frame malformed")
	errAuthFrameMissingToken = errors.New("ws: auth frame missing token")
)

// 与 gorilla/websocket 官方 chat 示例一致的推荐参数。
const (
	// writeWait 单次写入最大耗时；超过即判定为连接异常
	writeWait = 10 * time.Second

	// pongWait 收到 pong 前允许的最长空闲时间；超时即关闭连接
	pongWait = 60 * time.Second

	// pingPeriod 服务端主动发 ping 的周期；必须 < pongWait
	pingPeriod = 30 * time.Second

	// maxMessageSize 客户端上行消息大小上限（bytes）。
	// 只允许首帧 auth 和后续 pong，业务消息一律走 REST，无需大缓冲
	maxMessageSize = 1024

	// authTimeout 首帧 auth 必须在此时限内到达
	authTimeout = 2 * time.Second

	// sendBuffer 每个客户端 send channel 缓冲大小
	sendBuffer = 256
)

// Client 封装一个 WebSocket 连接的读写 pump。
//
// 生命周期：
//   - handler 层通过 NewClient 构造后，必须调用 Start() 启动两个 goroutine；
//     auth 成功前不 Register 到 Hub，失败直接 close
//   - 连接生命周期内由 readPump 和 writePump 共同持有 conn；任一方退出即关闭连接
//   - close() 保证 send channel 只被关闭一次，避免重复 close panic
type Client struct {
	conn   *websocket.Conn
	hub    *Hub
	send   chan []byte
	logger *log.Logger

	// closeOnce 保护 send channel 不被 close 两次。
	// readPump 和 writePump 都可能在 defer 里调用 close()
	closeOnce sync.Once
}

// NewClient 构造一个 Client。不做任何 IO，只初始化字段。
func NewClient(conn *websocket.Conn, hub *Hub, logger *log.Logger) *Client {
	if logger == nil {
		logger = log.Default()
	}
	return &Client{
		conn:   conn,
		hub:    hub,
		send:   make(chan []byte, sendBuffer),
		logger: logger,
	}
}

// authMessage 客户端首帧期望格式：{"type":"auth","token":"<jwt>"}
type authMessage struct {
	Type  string `json:"type"`
	Token string `json:"token"`
}

// authenticate 阻塞读取首帧，校验 token。
//
// 返回：
//   - 非空 actor 名 + nil err：auth 成功
//   - "" + 非 nil err：auth 失败（连接会被调用方以 CloseCodeUnauthorized 关闭）
//
// 超时由 conn.SetReadDeadline 控制；超时错误也算 auth 失败。
// 此方法只读一帧消息就返回，不进入 pump 循环。
func (c *Client) authenticate(validate func(token string) (string, error)) (string, error) {
	c.conn.SetReadLimit(maxMessageSize)
	if err := c.conn.SetReadDeadline(time.Now().Add(authTimeout)); err != nil {
		return "", err
	}

	_, data, err := c.conn.ReadMessage()
	if err != nil {
		return "", err
	}

	var msg authMessage
	if err := json.Unmarshal(data, &msg); err != nil {
		return "", errAuthFrameMalformed
	}
	if msg.Type != "auth" {
		return "", errAuthFrameMalformed
	}
	if msg.Token == "" {
		return "", errAuthFrameMissingToken
	}
	actor, err := validate(msg.Token)
	if err != nil {
		return "", err
	}
	// 清除 auth 超时 deadline，后续由 pong handler 重置
	if err := c.conn.SetReadDeadline(time.Time{}); err != nil {
		return "", err
	}
	return actor, nil
}

// sendReady 在 auth 成功后直接同步写一个 ready 帧，不走 send channel。
// 此时 writePump 还没启动，send channel 可能未被消费。
func (c *Client) sendReady() error {
	payload, _ := json.Marshal(map[string]any{
		"type":        EventReady,
		"server_time": time.Now().UTC().Format(time.RFC3339),
	})
	_ = c.conn.SetWriteDeadline(time.Now().Add(writeWait))
	return c.conn.WriteMessage(websocket.TextMessage, payload)
}

// Start 启动 readPump 与 writePump 两个 goroutine。
// 必须在 hub.Register 后调用（见 handler.go）。
func (c *Client) Start() {
	go c.writePump()
	go c.readPump()
}

// readPump 读协程：设置 pong handler + 丢弃所有业务上行消息。
// 任何读错误（含正常关闭）都会触发 unregister 并退出。
func (c *Client) readPump() {
	defer func() {
		c.hub.Unregister(c)
	}()

	c.conn.SetReadLimit(maxMessageSize)
	_ = c.conn.SetReadDeadline(time.Now().Add(pongWait))
	c.conn.SetPongHandler(func(string) error {
		return c.conn.SetReadDeadline(time.Now().Add(pongWait))
	})

	for {
		mt, _, err := c.conn.ReadMessage()
		if err != nil {
			if websocket.IsUnexpectedCloseError(err,
				websocket.CloseGoingAway, websocket.CloseAbnormalClosure,
				websocket.CloseNormalClosure) {
				c.logger.Printf("ws: read error: %v", err)
			}
			return
		}
		// 客户端只应发 ping/pong/close 控制帧；
		// 上送任何业务 text/binary message 都视为协议违规，断开连接
		if mt == websocket.TextMessage || mt == websocket.BinaryMessage {
			_ = c.conn.WriteControl(websocket.CloseMessage,
				websocket.FormatCloseMessage(CloseCodeBadFrame, "business messages not allowed"),
				time.Now().Add(writeWait))
			return
		}
	}
}

// writePump 写协程：
//   - 从 send channel 读业务事件帧并写入 conn
//   - 周期发 ping 保活
//   - send channel 被关闭时退出（优雅退出路径）
func (c *Client) writePump() {
	ticker := time.NewTicker(pingPeriod)
	defer func() {
		ticker.Stop()
		_ = c.conn.Close()
	}()

	for {
		select {
		case payload, ok := <-c.send:
			_ = c.conn.SetWriteDeadline(time.Now().Add(writeWait))
			if !ok {
				// send channel 被 close：Hub 主动踢下线
				_ = c.conn.WriteMessage(websocket.CloseMessage, []byte{})
				return
			}
			if err := c.conn.WriteMessage(websocket.TextMessage, payload); err != nil {
				c.logger.Printf("ws: write message failed: %v", err)
				return
			}
		case <-ticker.C:
			_ = c.conn.SetWriteDeadline(time.Now().Add(writeWait))
			if err := c.conn.WriteMessage(websocket.PingMessage, nil); err != nil {
				// ping 失败即认为连接已断，退出循环让 Hub 清理
				return
			}
		}
	}
}

// close 关闭 send channel 以通知 writePump 退出；幂等。
// 真正的底层 conn 关闭由 writePump 的 defer 负责。
func (c *Client) close() {
	c.closeOnce.Do(func() {
		close(c.send)
	})
}

// closeWithCode 在 auth 失败等场景下同步发送一个 Close 控制帧并关闭底层连接。
// 不经过 send channel（此时 writePump 通常还没启动）。
func (c *Client) closeWithCode(code int, reason string) {
	_ = c.conn.WriteControl(websocket.CloseMessage,
		websocket.FormatCloseMessage(code, reason),
		time.Now().Add(writeWait))
	_ = c.conn.Close()
}
