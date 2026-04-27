// Package main 是 todo-server 的入口：加载配置 → 打开 DB → 装配 services/handlers → 启动 HTTP。
//
// 二进制支持两种运行模式：
//  1. 默认（无参数）：作为 HTTP server 启动。
//  2. `-healthcheck`：作为单次健康探针运行——向本机 /health 发 GET 请求，2xx 退出 0，
//     其他情况退出 1。用于 distroless 运行时的 Docker HEALTHCHECK（distroless 没有
//     shell 或 wget/curl，只能调用静态二进制自身）。
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
)

// version 可在编译时通过 -ldflags "-X main.version=xxx" 注入。
var version = "dev"

func main() {
	// 先解析 flag —— healthcheck 模式不会进入正常的 server 启动流程。
	// 使用独立的 FlagSet 以便在 healthcheck 模式下 usage 输出清晰。
	fs := flag.NewFlagSet("todo-server", flag.ExitOnError)
	healthcheckMode := fs.Bool("healthcheck", false,
		"Probe http://127.0.0.1:${TODO_PORT}/health and exit 0 on 2xx, 1 otherwise. "+
			"Used by Docker HEALTHCHECK in distroless runtimes.")
	showVersion := fs.Bool("version", false, "Print version and exit.")
	if err := fs.Parse(os.Args[1:]); err != nil {
		// flag.ExitOnError 已自动处理，这里不会走到
		os.Exit(2)
	}

	if *showVersion {
		fmt.Println(version)
		return
	}

	if *healthcheckMode {
		os.Exit(runHealthcheck())
	}

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
	todoSvc, err := service.NewTodoService(todoRepo, auditSvc)
	if err != nil {
		logger.Fatalf("new todo service: %v", err)
	}
	stickyRepo, err := repository.NewStickyRepo(db)
	if err != nil {
		logger.Fatalf("new sticky repo: %v", err)
	}
	stickySvc, err := service.NewStickyService(stickyRepo, auditSvc)
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

// runHealthcheck 作为独立子命令执行：发一次 GET 到本机 /health，
// 2xx 返回 0，其他情况返回 1。
//
// 为什么不用 curl/wget：distroless/static 运行时不包含任何 shell 或工具链，
// Docker HEALTHCHECK 只能调用静态二进制——复用 server 自身最自然，
// 且探针逻辑与 server /health 端点实现同步演进，不会漂移。
//
// 超时上限 3s——Docker 默认 healthcheck timeout 是 30s，这里留足余量。
// 探针只看 HTTP 状态码，不解析 body，避免与 /health 响应结构耦合。
func runHealthcheck() int {
	port := strings.TrimSpace(os.Getenv("TODO_PORT"))
	if port == "" {
		port = "8080"
	}
	// 探测 127.0.0.1 —— healthcheck 由容器内同进程执行，loopback 最稳。
	url := fmt.Sprintf("http://127.0.0.1:%s/health", port)

	client := &http.Client{
		Timeout: 3 * time.Second,
	}
	resp, err := client.Get(url)
	if err != nil {
		fmt.Fprintf(os.Stderr, "healthcheck: GET %s failed: %v\n", url, err)
		return 1
	}
	defer func() { _ = resp.Body.Close() }()

	if resp.StatusCode >= 200 && resp.StatusCode < 300 {
		return 0
	}
	fmt.Fprintf(os.Stderr, "healthcheck: GET %s returned %d\n", url, resp.StatusCode)
	return 1
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
