package config

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"time"
)

// Config 保存服务端运行时配置。所有字段均从环境变量加载。
//
// 注意：
//   - JWT 签名密钥不在此结构里——它由 server 启动时从 SQLite 的 app_secrets 表
//     读取；若不存在则自动生成 32 字节随机值并持久化，详见
//     internal/repository/secret_repo.go#GetOrCreateJWTSecret。这样可以避免用户
//     手动管理密钥，同时保证 token 跨重启有效。
//   - `TODO_PORT` / `TODO_USERNAME` / `TODO_PASSWORD` 支持通过 CLI flag
//     `-port` / `-username` / `-password` 覆盖；flag 在 cmd/todo-server/main.go
//     里通过 os.Setenv 写回同名环境变量后再调用 Load，因此这里看到的仍是
//     "从环境变量读取"，但最终生效值可能来自 flag（flag 优先级更高）。
type Config struct {
	// Port 监听端口，环境变量 TODO_PORT，默认 "8080"。必须是 1-65535 的整数。
	Port string
	// Username 单账号用户名，环境变量 TODO_USERNAME，必填。
	Username string
	// Password 单账号密码，环境变量 TODO_PASSWORD，必填。
	Password string
	// DataDir SQLite 数据目录，环境变量 TODO_DATA_DIR，默认 "./data"。
	DataDir string
	// TokenTTL JWT 有效期，环境变量 TODO_TOKEN_TTL（支持 time.ParseDuration 格式），默认 24h。
	TokenTTL time.Duration
	// GinMode Gin 运行模式，环境变量 TODO_GIN_MODE，默认 "release"。可选：debug / release / test。
	GinMode string
	// Verbose 是否开启详细日志（影响 GORM 日志级别），环境变量 TODO_VERBOSE，默认 false。
	// 接受 1/true/yes/on 视为 true，其他一律 false。
	Verbose bool
}

// Load 从进程环境变量加载配置，若必填字段缺失或格式非法则返回错误。
func Load() (*Config, error) {
	cfg := &Config{
		Port:     getenvDefault("TODO_PORT", "8080"),
		Username: strings.TrimSpace(os.Getenv("TODO_USERNAME")),
		Password: os.Getenv("TODO_PASSWORD"),
		DataDir:  getenvDefault("TODO_DATA_DIR", "./data"),
		GinMode:  getenvDefault("TODO_GIN_MODE", "release"),
	}

	// 解析并校验 Verbose（非法值必须报错，不静默回退）
	verbose, err := parseBoolEnv("TODO_VERBOSE", false)
	if err != nil {
		return nil, err
	}
	cfg.Verbose = verbose

	if cfg.Username == "" {
		return nil, errors.New("TODO_USERNAME is required")
	}
	if cfg.Password == "" {
		return nil, errors.New("TODO_PASSWORD is required")
	}

	// 校验 Port 是合法端口号 (1-65535)
	portNum, portErr := strconv.Atoi(cfg.Port)
	if portErr != nil {
		return nil, fmt.Errorf("invalid TODO_PORT %q: must be integer", cfg.Port)
	}
	if portNum < 1 || portNum > 65535 {
		return nil, fmt.Errorf("invalid TODO_PORT %d: must be in range 1-65535", portNum)
	}

	// 校验 GinMode
	switch cfg.GinMode {
	case "debug", "release", "test":
	default:
		return nil, fmt.Errorf("invalid TODO_GIN_MODE %q: must be one of debug/release/test", cfg.GinMode)
	}

	// 解析 TokenTTL
	ttlStr := strings.TrimSpace(os.Getenv("TODO_TOKEN_TTL"))
	if ttlStr == "" {
		cfg.TokenTTL = 24 * time.Hour
	} else {
		d, err := time.ParseDuration(ttlStr)
		if err != nil {
			return nil, fmt.Errorf("invalid TODO_TOKEN_TTL %q: %w", ttlStr, err)
		}
		if d <= 0 {
			return nil, fmt.Errorf("TODO_TOKEN_TTL must be positive, got %s", d)
		}
		cfg.TokenTTL = d
	}

	return cfg, nil
}

// DBPath 返回 SQLite 数据库文件完整路径。使用 filepath.Join 处理跨平台分隔符。
func (c *Config) DBPath() string {
	return filepath.Join(c.DataDir, "todo.db")
}

// Addr 返回 Gin 监听地址，形如 ":8080"。
func (c *Config) Addr() string {
	return ":" + c.Port
}

// SafeString 返回 Config 的安全文本表示，屏蔽 Password 等敏感字段，
// 用于启动日志打印。JWT 密钥不再属于 Config，由 repository.GetOrCreateJWTSecret
// 从 DB 管理，因此这里不打印。
func (c *Config) SafeString() string {
	return fmt.Sprintf(
		"Config{Port:%s, Username:%s, Password:***, DataDir:%s, TokenTTL:%s, GinMode:%s, Verbose:%t}",
		c.Port, c.Username, c.DataDir, c.TokenTTL, c.GinMode, c.Verbose,
	)
}

// String 重写默认字符串表示，避免 %v / %+v 意外泄漏敏感字段。
func (c *Config) String() string {
	return c.SafeString()
}

func getenvDefault(key, def string) string {
	v := strings.TrimSpace(os.Getenv(key))
	if v == "" {
		return def
	}
	return v
}

// parseBoolEnv 解析环境变量的布尔值表示。
//   - 未设置（空字符串）返回 def 且 err==nil
//   - 可识别的真/假关键字返回对应布尔值
//   - 其他非法值返回错误（而非静默回退），避免拼写错误导致行为与预期不符
func parseBoolEnv(key string, def bool) (bool, error) {
	v := strings.ToLower(strings.TrimSpace(os.Getenv(key)))
	if v == "" {
		return def, nil
	}
	switch v {
	case "1", "true", "yes", "on", "t", "y":
		return true, nil
	case "0", "false", "no", "off", "f", "n":
		return false, nil
	default:
		return false, fmt.Errorf("invalid boolean for %s=%q: expect one of 1/0/true/false/yes/no/on/off", key, v)
	}
}

