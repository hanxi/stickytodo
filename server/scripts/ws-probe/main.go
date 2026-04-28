// Package main 是 ws-probe：一个极简的 WebSocket 行为校验工具，专为
// smoke.sh 的 Step 33-36 设计，用来黑盒验证 /api/ws 的握手 / auth / 事件推送
// 三类行为。
//
// 为什么独立成一个 Go 程序而不是在 bash 里用 websocat / curl：
//  1. smoke.sh 已明确"零外部依赖"原则（不能要求 CI / 开发机装 websocat / wscat）；
//     本工具直接复用 server 模块里的 github.com/gorilla/websocket，和后端使用同一
//     实现，框架级的 close code / ping-pong 行为完全一致，避免跨实现差异引入假阳性；
//  2. 把 4401 / 4400 这类应用级 close code 的断言放在 Go 侧，比在 bash 里解析文本
//     可靠得多；
//  3. 保持 go run ./scripts/ws-probe 即可执行，不产生额外的构建产物或部署负担。
//
// 退出码约定（与 smoke.sh 读取相互锁定，改动需同步）：
//   - 0: 所有断言通过
//   - 1: 连接 / 断言失败（被测对象行为异常）
//   - 2: 参数错误（调用方/脚本错误）
//
// Mode 一览：
//   - no-auth   : 连上后故意**不**发 auth 首帧，等待服务端在 authTimeout 内关连接，
//                 断言其 close code 等于 -expect-code（通常 4401）
//   - bad-token : 连上后发送一个合法结构但 token 非法的 auth 首帧，断言服务端以
//                 -expect-code（通常 4401）关闭
//   - auth-ready: 正常 auth，断言收到 {"type":"ready"} 帧
//   - wait-event: 正常 auth + ready 后，继续等待任意 -expect-type（如 "todo.created"）
//                 事件出现；用于和主测试脚本"另起线程 POST REST，看 WS 是否推送"
//                 的断言配合
package main

import (
	"context"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"net/http"
	"net/url"
	"os"
	"time"

	"github.com/gorilla/websocket"
)

// 退出码常量，避免魔法数散落各处。main 内部固定用 os.Exit 返回这些值。
const (
	exitOK        = 0
	exitFail      = 1
	exitUsageErr  = 2
)

// fatal 在参数校验 / 不可继续的错误场景统一打印到 stderr 并以指定 code 退出。
// 使用 os.Exit 绕过 defer，是因为本程序没有需要清理的外部状态（连接由 defer 关闭，
// 在 main 正常返回路径里已经完成）。
func fatal(code int, format string, args ...any) {
	fmt.Fprintf(os.Stderr, "ws-probe: "+format+"\n", args...)
	os.Exit(code)
}

func main() {
	fs := flag.NewFlagSet("ws-probe", flag.ContinueOnError)
	urlStr := fs.String("url", "", "WebSocket URL，例如 ws://127.0.0.1:8080/api/ws（必填）")
	token := fs.String("token", "", "JWT token，仅 bad-token/auth-ready/wait-event 模式用")
	mode := fs.String("mode", "", "探测模式：no-auth | bad-token | auth-ready | wait-event（必填）")
	expectCode := fs.Int("expect-code", 0, "期望的 WebSocket close code（no-auth / bad-token 模式必填）")
	expectType := fs.String("expect-type", "", "期望收到的事件 type（wait-event 模式必填）")
	timeout := fs.Duration("timeout", 5*time.Second, "整个探测的总超时")
	if err := fs.Parse(os.Args[1:]); err != nil {
		// flag.ContinueOnError 下 Parse 自身已经打印了错误说明
		os.Exit(exitUsageErr)
	}

	if *urlStr == "" {
		fatal(exitUsageErr, "-url is required")
	}
	if *mode == "" {
		fatal(exitUsageErr, "-mode is required")
	}
	if _, err := url.Parse(*urlStr); err != nil {
		fatal(exitUsageErr, "invalid -url: %v", err)
	}

	// 所有模式都挂在这个 ctx 上，保证"整个探测不超过 -timeout"的硬边界。
	// 内部 Read / Dial 都使用 SetReadDeadline 配合，而非依赖 ctx 传递，因为
	// gorilla/websocket 的 Read API 是基于 deadline 而非 ctx 的。
	ctx, cancel := context.WithTimeout(context.Background(), *timeout)
	defer cancel()

	switch *mode {
	case "no-auth":
		if *expectCode == 0 {
			fatal(exitUsageErr, "mode=no-auth requires -expect-code")
		}
		runNoAuth(ctx, *urlStr, *expectCode, *timeout)
	case "bad-token":
		if *expectCode == 0 {
			fatal(exitUsageErr, "mode=bad-token requires -expect-code")
		}
		if *token == "" {
			// 这里 token 虽然会被拒，但仍要求脚本显式传一个占位，避免误用
			fatal(exitUsageErr, "mode=bad-token requires -token (any intentionally-invalid value)")
		}
		runBadToken(ctx, *urlStr, *token, *expectCode, *timeout)
	case "auth-ready":
		if *token == "" {
			fatal(exitUsageErr, "mode=auth-ready requires -token")
		}
		runAuthReady(ctx, *urlStr, *token, *timeout)
	case "wait-event":
		if *token == "" {
			fatal(exitUsageErr, "mode=wait-event requires -token")
		}
		if *expectType == "" {
			fatal(exitUsageErr, "mode=wait-event requires -expect-type")
		}
		runWaitEvent(ctx, *urlStr, *token, *expectType, *timeout)
	default:
		fatal(exitUsageErr, "unknown mode %q (allowed: no-auth | bad-token | auth-ready | wait-event)", *mode)
	}
}

// dial 执行 WS Upgrade。单独抽出是因为四个模式都要做同一件事，而错误分类需要
// 一致：Dial 失败一律作为 exitFail（被测对象异常 / 网络异常），不是参数错。
func dial(ctx context.Context, urlStr string) (*websocket.Conn, error) {
	dialer := *websocket.DefaultDialer
	// 2s 足够本地连接握手；复用整体 timeout 可能过短时握手就被 cut，所以用独立上限。
	dialer.HandshakeTimeout = 2 * time.Second

	conn, resp, err := dialer.DialContext(ctx, urlStr, http.Header{})
	if err != nil {
		if resp != nil {
			return nil, fmt.Errorf("dial: %w (http status=%d)", err, resp.StatusCode)
		}
		return nil, fmt.Errorf("dial: %w", err)
	}
	return conn, nil
}

// sendAuth 发送首帧 auth 消息。handler 侧要求格式严格为 {"type":"auth","token":"..."}。
// 这里通过 json.Marshal 而不是字符串拼接，避免 token 含特殊字符时被错误转义。
func sendAuth(conn *websocket.Conn, token string) error {
	frame := struct {
		Type  string `json:"type"`
		Token string `json:"token"`
	}{Type: "auth", Token: token}
	raw, err := json.Marshal(frame)
	if err != nil {
		return fmt.Errorf("marshal auth frame: %w", err)
	}
	// WriteMessage 使用默认写超时（无），在 handshake 成功后的短时间内
	// 不太可能阻塞；即便阻塞，外层 ctx.Deadline 也通过 SetWriteDeadline 控制。
	if deadline, ok := ctxDeadline(conn, 2*time.Second); ok {
		_ = conn.SetWriteDeadline(deadline)
	}
	return conn.WriteMessage(websocket.TextMessage, raw)
}

// ctxDeadline 给 WS 读写设置一个相对保守的 deadline。gorilla/websocket 的
// Set*Deadline 只接受绝对时间，不读 context。这里统一做转换，避免在每个
// 调用点手写 time.Now().Add(...)。
//
// 返回值的第二个 bool 表示"是否设置了 deadline"；conn==nil 或 fallback 为 0 时
// 返回 false 以便调用方跳过。
func ctxDeadline(conn *websocket.Conn, fallback time.Duration) (time.Time, bool) {
	if conn == nil || fallback <= 0 {
		return time.Time{}, false
	}
	return time.Now().Add(fallback), true
}

// expectClose 读消息直到连接被对端关闭，断言对端的 close code。
//
// 行为：
//  1. 反复 ReadMessage 直到拿到 *websocket.CloseError
//  2. 拿到后比对 code，吻合则返回 nil（由 main 正常退出 0）
//  3. code 不符或读出的是非 close 帧但连接未关，返回错误
//
// 读超时由 waitFor 决定，waitFor 应比 server 侧的 authTimeout(=2s) 宽裕
// 一些（留握手抖动 buffer）。
func expectClose(conn *websocket.Conn, wantCode int, waitFor time.Duration) error {
	deadline := time.Now().Add(waitFor)
	if err := conn.SetReadDeadline(deadline); err != nil {
		return fmt.Errorf("set read deadline: %w", err)
	}
	for {
		_, _, err := conn.ReadMessage()
		if err == nil {
			// server 此时应该在 close，不应当收到业务帧；收到就是异常
			return errors.New("unexpected text/binary frame before close")
		}
		var ce *websocket.CloseError
		if errors.As(err, &ce) {
			if ce.Code == wantCode {
				return nil
			}
			return fmt.Errorf("close code mismatch: got %d (%q), want %d", ce.Code, ce.Text, wantCode)
		}
		// 非 CloseError 的错误（含 deadline 超时）：服务端没按预期关连接。
		return fmt.Errorf("expected close, got error: %w", err)
	}
}

// runNoAuth：连上后什么也不发，等 server 在 authTimeout 内关连接。
// waitFor 比 handler.go 里的 authTimeout(2s) 多留一倍 buffer。
func runNoAuth(ctx context.Context, urlStr string, wantCode int, totalTimeout time.Duration) {
	conn, err := dial(ctx, urlStr)
	if err != nil {
		fatal(exitFail, "%v", err)
	}
	defer conn.Close()

	// 给 server 的 auth 超时留足够时间：取 min(totalTimeout, 4s) 作为读窗口。
	waitFor := totalTimeout
	if waitFor > 4*time.Second {
		waitFor = 4 * time.Second
	}
	if err := expectClose(conn, wantCode, waitFor); err != nil {
		fatal(exitFail, "no-auth: %v", err)
	}
	os.Exit(exitOK)
}

// runBadToken：发一个格式合法但 token 非法的 auth 帧，等 server 按 4401 关闭。
func runBadToken(ctx context.Context, urlStr, token string, wantCode int, totalTimeout time.Duration) {
	conn, err := dial(ctx, urlStr)
	if err != nil {
		fatal(exitFail, "%v", err)
	}
	defer conn.Close()

	if err := sendAuth(conn, token); err != nil {
		fatal(exitFail, "send auth: %v", err)
	}

	waitFor := totalTimeout
	if waitFor > 4*time.Second {
		waitFor = 4 * time.Second
	}
	if err := expectClose(conn, wantCode, waitFor); err != nil {
		fatal(exitFail, "bad-token: %v", err)
	}
	os.Exit(exitOK)
}

// readEvent 读取一条 JSON 事件帧并反序列化为 (type, data 原始 JSON)。
// 非文本帧 / JSON 解析失败都视为被测对象异常。
func readEvent(conn *websocket.Conn, waitFor time.Duration) (string, json.RawMessage, error) {
	deadline := time.Now().Add(waitFor)
	if err := conn.SetReadDeadline(deadline); err != nil {
		return "", nil, fmt.Errorf("set read deadline: %w", err)
	}
	msgType, payload, err := conn.ReadMessage()
	if err != nil {
		return "", nil, fmt.Errorf("read: %w", err)
	}
	if msgType != websocket.TextMessage {
		return "", nil, fmt.Errorf("unexpected message type %d (want text)", msgType)
	}
	// 事件结构与 ws.Event 对齐；只取 type，其余字段原样保留便于上层打印/调试。
	var env struct {
		Type string          `json:"type"`
		Data json.RawMessage `json:"data,omitempty"`
		ID   json.RawMessage `json:"id,omitempty"`
	}
	if err := json.Unmarshal(payload, &env); err != nil {
		return "", nil, fmt.Errorf("unmarshal event: %w (raw=%s)", err, string(payload))
	}
	if env.Type == "" {
		return "", nil, fmt.Errorf("event missing type field (raw=%s)", string(payload))
	}
	return env.Type, env.Data, nil
}

// runAuthReady：正常 auth，断言收到 {"type":"ready"}。
func runAuthReady(ctx context.Context, urlStr, token string, totalTimeout time.Duration) {
	conn, err := dial(ctx, urlStr)
	if err != nil {
		fatal(exitFail, "%v", err)
	}
	defer conn.Close()

	if err := sendAuth(conn, token); err != nil {
		fatal(exitFail, "send auth: %v", err)
	}
	// 3s 足够 server 处理 auth；若 totalTimeout 更小则按 totalTimeout。
	waitFor := totalTimeout
	if waitFor > 3*time.Second {
		waitFor = 3 * time.Second
	}
	typ, _, err := readEvent(conn, waitFor)
	if err != nil {
		fatal(exitFail, "auth-ready: %v", err)
	}
	if typ != "ready" {
		fatal(exitFail, "auth-ready: expected type=ready, got %q", typ)
	}
	os.Exit(exitOK)
}

// runWaitEvent：auth → ready 之后，继续读直到遇到 expectType；遇到不同类型
// 的事件时**不退出**（实时系统里可能先收到无关事件），仅当整体 ctx 超时时报错。
//
// 这样设计的好处：smoke.sh 可以在后台起 probe，然后主线程发 REST 请求，不必关心
// 两边的时序——只要在 totalTimeout 内 WS 推送过来，probe 就会成功退出。
func runWaitEvent(ctx context.Context, urlStr, token, expectType string, totalTimeout time.Duration) {
	conn, err := dial(ctx, urlStr)
	if err != nil {
		fatal(exitFail, "%v", err)
	}
	defer conn.Close()

	if err := sendAuth(conn, token); err != nil {
		fatal(exitFail, "send auth: %v", err)
	}

	// 先吞掉 ready 帧。ready 是协议保证的首帧，读不到就是握手异常。
	// 给 ready 最多 3s 或 totalTimeout 的较小值。
	readyWait := totalTimeout
	if readyWait > 3*time.Second {
		readyWait = 3 * time.Second
	}
	typ, _, err := readEvent(conn, readyWait)
	if err != nil {
		fatal(exitFail, "wait-event: failed before ready: %v", err)
	}
	if typ != "ready" {
		// 允许 server 在 ready 之前就推送事件吗？当前 handler 的契约是 ready 必先，
		// 所以严格断言。若未来协议放宽，这里可以改成 "匹配就结束/不匹配就继续"。
		fatal(exitFail, "wait-event: expected ready first, got %q", typ)
	}

	// 向 stdout 输出 "READY\n" 作为握手完成信号。这是 smoke.sh 和 ws-probe 的协议：
	// smoke.sh 会先从 probe stdout 读一行 "READY" 再发 REST 触发请求，以此彻底消除
	// "sleep N 秒等 probe 握手完成" 的经验时序假设。
	//
	// 为什么不需要 Flush / Sync：Go 的 os.Stdout 是裸 *os.File，没有用户态缓冲；
	// WriteString 底层就是 write(2) 系统调用，成功返回即表示字节已经进入内核的
	// pipe buffer，对端 read 立即可见。Sync 在 pipe 上会返回 EINVAL（pipe 不支持
	// fsync），徒增一条 stderr 噪声。
	if _, err := os.Stdout.WriteString("READY\n"); err != nil {
		// Stdout 写失败意味着 smoke.sh 那端的 pipe 已关；此时继续读事件也没意义
		fatal(exitFail, "wait-event: write READY signal: %v", err)
	}

	// 之后在剩余时间里反复读，直到遇到目标事件或总超时。
	// 每次 readEvent 用一个稍短的窗口（1.5s），避免单次 Read 阻塞到总超时末端
	// 时还未必能观察到 ctx.Done()——gorilla/websocket 的 Read 只看 SetReadDeadline。
	for {
		select {
		case <-ctx.Done():
			fatal(exitFail, "wait-event: timeout waiting for %q", expectType)
		default:
		}

		window := 1500 * time.Millisecond
		if d, ok := ctxRemaining(ctx); ok && d < window {
			window = d
		}
		if window <= 0 {
			fatal(exitFail, "wait-event: timeout waiting for %q", expectType)
		}

		typ, _, err := readEvent(conn, window)
		if err != nil {
			// 超时很正常（还没收到目标事件），继续循环让 select 的 ctx 分支判断是否
			// 真的到总超时。非超时错（如连接断开）才 fatal。
			var netErr interface{ Timeout() bool }
			if errors.As(err, &netErr) && netErr.Timeout() {
				continue
			}
			fatal(exitFail, "wait-event: %v", err)
		}
		if typ == expectType {
			os.Exit(exitOK)
		}
		// 忽略非目标事件，继续等。
	}
}

// ctxRemaining 返回 ctx 剩余时间，ctx 无 deadline 时返回 (0,false)。
func ctxRemaining(ctx context.Context) (time.Duration, bool) {
	dl, ok := ctx.Deadline()
	if !ok {
		return 0, false
	}
	return time.Until(dl), true
}
