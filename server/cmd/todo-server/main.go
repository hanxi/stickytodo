// Package main 是 todo-server 的入口：加载配置 → 打开 DB → 装配 services/handlers → 启动 HTTP。
//
// 二进制支持以下运行模式 / 参数：
//  1. 默认（无参数）：作为 HTTP server 启动。
//  2. `-version`：打印由 -ldflags 注入的 main.version 并退出。
//  3. `-port` / `-username` / `-password`：分别覆盖环境变量 TODO_PORT / TODO_USERNAME /
//     TODO_PASSWORD；flag 非空时优先级高于环境变量，留空则完全回退到环境变量（或默认值）。
//     覆盖通过 os.Setenv 注入，后续 config.Load 统一做校验，避免校验规则漂移。
package main

import (
	"context"
	"errors"
	"flag"
	"fmt"
	"log"
	"net/http"
	"os"
	"os/signal"
	"strings"
	"syscall"
	"time"

	"github.com/gin-gonic/gin"

	"github.com/hanxi/todo-server/internal/config"
	"github.com/hanxi/todo-server/internal/handler"
	"github.com/hanxi/todo-server/internal/model"
	"github.com/hanxi/todo-server/internal/repository"
	"github.com/hanxi/todo-server/internal/router"
	"github.com/hanxi/todo-server/internal/service"
	"github.com/hanxi/todo-server/internal/ws"
)

// version 可在编译时通过 -ldflags "-X main.version=xxx" 注入。
var version = "dev"

func main() {
	// 使用独立的 FlagSet 以便 -help 输出清晰、便于测试。
	fs := flag.NewFlagSet("todo-server", flag.ExitOnError)
	showVersion := fs.Bool("version", false, "Print version and exit.")
	// 下述三个 flag 与对应的 TODO_* 环境变量等价；传入非空值时覆盖环境变量。
	// 默认值保留空串以便区分"用户未指定"和"用户显式指定空串"——后者会被 trim 掉，
	// 和未指定的效果一致，均走 config.Load 的环境变量/默认值逻辑。
	portFlag := fs.String("port", "", "Override TODO_PORT (listening port, 1-65535).")
	usernameFlag := fs.String("username", "", "Override TODO_USERNAME (single account username).")
	passwordFlag := fs.String("password", "", "Override TODO_PASSWORD (single account password).")
	if err := fs.Parse(os.Args[1:]); err != nil {
		// flag.ExitOnError 已自动处理，这里不会走到
		os.Exit(2)
	}

	if *showVersion {
		fmt.Println(version)
		return
	}

	// CLI flag 优先级高于环境变量：非空的 flag 会覆盖同名 TODO_* 环境变量，
	// 再交给 config.Load 统一走校验逻辑（端口范围、必填项、SafeString 打印等），
	// 避免校验规则在 main.go 和 config.go 两处重复。
	applyFlagOverride("TODO_PORT", *portFlag)
	applyFlagOverride("TODO_USERNAME", *usernameFlag)
	applyFlagOverride("TODO_PASSWORD", *passwordFlag)

	logger := log.New(os.Stdout, "[todo-server] ", log.LstdFlags|log.Lmsgprefix)

	cfg, err := config.Load()
	if err != nil {
		logger.Fatalf("load config: %v", err)
	}
	gin.SetMode(cfg.GinMode)
	logger.Printf("starting version=%s %s", version, cfg.SafeString())

	// 打开 DB
	db, err := model.Open(model.OpenOptions{
		DataDir: cfg.DataDir,
		Verbose: cfg.Verbose,
	})
	if err != nil {
		logger.Fatalf("open db: %v", err)
	}
	logger.Printf("db opened at %s", cfg.DBPath())

	// 读取或自动生成 JWT 签名密钥（持久化到 app_secrets 表，跨重启保持不变）。
	// 这里必须在 NewAuthService 之前完成——AuthService 构造时需要 secret。
	jwtSecret, err := repository.GetOrCreateJWTSecret(db)
	if err != nil {
		logger.Fatalf("load or create jwt secret: %v", err)
	}
	logger.Printf("jwt secret loaded (%d chars) from app_secrets table", len(jwtSecret))

	// 构造 WebSocket Hub 与 broadcaster：
	//   - Hub 维护所有已鉴权客户端连接，提供 Broadcast 扇出
	//   - HubBroadcaster 实现 service.EventBroadcaster 语义接口，把
	//     TodoService/StickyService 的业务事件翻译成 WS 事件帧
	// Hub 必须先于 service 构造，才能作为依赖注入；顺序：Hub → services → router。
	hub := ws.NewHub(logger)
	go hub.Run() // 当前 Run() 是 no-op，保留 go 形式预留未来中心化派发扩展点
	broadcaster := ws.NewHubBroadcaster(hub, logger)

	// 构造 services
	authSvc, err := service.NewAuthService(cfg.Username, cfg.Password, jwtSecret, cfg.TokenTTL)
	if err != nil {
		logger.Fatalf("new auth service: %v", err)
	}
	auditSvc, err := service.NewAuditService(db, logger)
	if err != nil {
		logger.Fatalf("new audit service: %v", err)
	}
	todoRepo, err := repository.NewTodoRepo(db)
	if err != nil {
		logger.Fatalf("new todo repo: %v", err)
	}
	todoSvc, err := service.NewTodoService(todoRepo, auditSvc, broadcaster)
	if err != nil {
		logger.Fatalf("new todo service: %v", err)
	}
	stickyRepo, err := repository.NewStickyRepo(db)
	if err != nil {
		logger.Fatalf("new sticky repo: %v", err)
	}
	stickySvc, err := service.NewStickyService(stickyRepo, auditSvc, broadcaster)
	if err != nil {
		logger.Fatalf("new sticky service: %v", err)
	}

	// 构造 handlers
	authH, err := handler.NewAuthHandler(authSvc, auditSvc)
	if err != nil {
		logger.Fatalf("new auth handler: %v", err)
	}
	todoH, err := handler.NewTodoHandler(todoSvc)
	if err != nil {
		logger.Fatalf("new todo handler: %v", err)
	}
	auditH, err := handler.NewAuditHandler(auditSvc, todoSvc)
	if err != nil {
		logger.Fatalf("new audit handler: %v", err)
	}
	tagH, err := handler.NewTagHandler(todoSvc)
	if err != nil {
		logger.Fatalf("new tag handler: %v", err)
	}
	stickyH, err := handler.NewStickyHandler(stickySvc)
	if err != nil {
		logger.Fatalf("new sticky handler: %v", err)
	}

	// 构造 router
	corsOrigins := parseCorsOrigins(os.Getenv("TODO_CORS_ORIGINS"))
	engine, err := router.Build(&router.Deps{
		Auth:        authSvc,
		Todos:       todoSvc,
		Audit:       auditSvc,
		Stickies:    stickySvc,
		AuthH:       authH,
		TodoH:       todoH,
		AuditH:      auditH,
		TagH:        tagH,
		StickyH:     stickyH,
		WSHub:       hub,
		CorsOrigins: corsOrigins,
		Logger:      logger,
		Version:     version,
	})
	if err != nil {
		logger.Fatalf("build router: %v", err)
	}

	// 启动 HTTP 服务（带 graceful shutdown）
	srv := &http.Server{
		Addr:              cfg.Addr(),
		Handler:           engine,
		ReadHeaderTimeout: 10 * time.Second,
		ReadTimeout:       30 * time.Second,
		WriteTimeout:      30 * time.Second,
		IdleTimeout:       120 * time.Second,
	}

	// 使用 SIGINT/SIGTERM 触发优雅退出
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()

	// 启动 server：通过 channel 把启动错误传回主 goroutine，
	// 避免在 goroutine 里 logger.Fatalf 跳过 defer 和 DB 关闭。
	serverErrCh := make(chan error, 1)
	go func() {
		logger.Printf("listening on %s (gin-mode=%s)", cfg.Addr(), cfg.GinMode)
		if err := srv.ListenAndServe(); err != nil && !errors.Is(err, http.ErrServerClosed) {
			serverErrCh <- err
			return
		}
		serverErrCh <- nil
	}()

	exitCode := 0
	select {
	case <-ctx.Done():
		logger.Printf("shutdown signal received, draining...")
	case err := <-serverErrCh:
		if err != nil {
			logger.Printf("listen error: %v", err)
			exitCode = 1
		}
	}

	shutdownCtx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
	defer cancel()

	if err := srv.Shutdown(shutdownCtx); err != nil {
		logger.Printf("graceful shutdown failed: %v", err)
		exitCode = 1
	}

	// 关闭 WebSocket Hub。位置放在 srv.Shutdown 之后、db.Close 之前，
	// 但必须清楚两个和直觉不符的事实：
	//
	// 1) srv.Shutdown 并**不会**等待已升级的 WebSocket 连接。gorilla/websocket
	//    在 Upgrade 完成后会 Hijack 底层 TCP 连接，Gin 的 HandlerFunc 随即返回；
	//    net/http.Server 从此不再感知这些连接。所以 srv.Shutdown 只能保证
	//    "没有新的 HTTP 请求在处理中"，不代表"所有 WS 都已关"。
	//
	// 2) 即便如此，仍有竞态安全性保证——hub.Close 会把 Hub.closed 置为 true，
	//    后续极少见的 in-flight upgrade（handler 已开始执行但还没 Register）
	//    走到 Hub.Register 时会命中 closed 分支，直接 close 新 client，不会
	//    泄漏或 panic（见 ws.Hub.Register 的 closed 守卫）。
	//
	// 3) Hub.Close 关闭所有已注册客户端的 send channel，writePump 据此写一个
	//    CloseMessage 并退出。这些 goroutine 不会被这里 wait——进程即将退出，
	//    runtime 会统一清理，有限的写超时（writeWait=10s）也保证不会卡住整个
	//    shutdown 超过预期。
	//
	// 4) Broadcaster 是 Hub 的薄适配器，自身无状态，无需单独 Close。
	hub.Close()

	// 关闭底层 DB 连接（即使前面出错也要尝试）
	if sqlDB, err := db.DB(); err == nil {
		if closeErr := sqlDB.Close(); closeErr != nil {
			logger.Printf("db close failed: %v", closeErr)
			exitCode = 1
		}
	} else {
		logger.Printf("get sql.DB failed: %v", err)
		exitCode = 1
	}

	logger.Printf("bye")
	if exitCode != 0 {
		os.Exit(exitCode)
	}
}

// applyFlagOverride 把 CLI flag 值覆盖到同名环境变量上，实现"flag 优先级高于环境变量"。
//
// 语义：
//   - value 为空串（包括全空白）视为"用户未传此 flag"，保留现有环境变量不动
//   - value 非空时 TrimSpace 后调用 os.Setenv，后续 config.Load 读到的即为该值
//
// 为什么选择"覆盖环境变量"而不是在 config.Load 之后再打补丁：
//  1. config.Load 已经集中处理了校验（端口范围、必填、Trim 等），两种来源走同一条校验路径，
//     避免校验逻辑在 main.go 和 config.go 两处漂移；
//  2. 子进程与现有调试工具（如 TODO_VERBOSE=true 打印的 os.Environ）都能看到真实生效的值。
//
// os.Setenv 的错误只会在 key 含 '=' 或 NUL 时出现，这里 key 是硬编码常量，不会触发；
// 但出于严谨仍打印到 stderr 并终止——忽略 Setenv 失败会导致后续 config.Load
// 拿到旧值、与用户预期不一致，属于"静默错误"，必须避免。
func applyFlagOverride(envKey, value string) {
	v := strings.TrimSpace(value)
	if v == "" {
		return
	}
	if err := os.Setenv(envKey, v); err != nil {
		fmt.Fprintf(os.Stderr, "failed to override %s via flag: %v\n", envKey, err)
		os.Exit(2)
	}
}

// parseCorsOrigins 解析逗号分隔的 origin 列表，去掉空白项。
func parseCorsOrigins(raw string) []string {
	if strings.TrimSpace(raw) == "" {
		return nil
	}
	parts := strings.Split(raw, ",")
	out := make([]string, 0, len(parts))
	for _, p := range parts {
		p = strings.TrimSpace(p)
		if p != "" {
			out = append(out, p)
		}
	}
	return out
}
