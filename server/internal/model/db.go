package model

import (
	"fmt"
	"log"
	"os"
	"path/filepath"
	"time"

	"github.com/glebarez/sqlite"
	"gorm.io/gorm"
	"gorm.io/gorm/logger"
)

// OpenOptions 控制数据库打开行为。
type OpenOptions struct {
	// DataDir 存放 sqlite 文件的目录。必填。
	DataDir string
	// DBFileName 数据库文件名，默认 "todo.db"。
	DBFileName string
	// Verbose 是否开启 GORM 详细日志（Info 级别）。默认 Warn 级别。
	Verbose bool
}

// Open 打开/创建 SQLite 数据库，执行 AutoMigrate。
// 会确保 DataDir 存在（权限 0755）。
func Open(opts OpenOptions) (*gorm.DB, error) {
	if opts.DataDir == "" {
		return nil, fmt.Errorf("DataDir is required")
	}
	if opts.DBFileName == "" {
		opts.DBFileName = "todo.db"
	}
	if err := os.MkdirAll(opts.DataDir, 0o755); err != nil {
		return nil, fmt.Errorf("create data dir %q: %w", opts.DataDir, err)
	}

	dbFile := filepath.Join(opts.DataDir, opts.DBFileName)
	// 开启 FK 约束、WAL 日志模式、5s 忙等待。
	// 切到 modernc.org/sqlite（纯 Go，无 CGO）后，DSN 语法从 mattn 的 `?_fk=1&_journal_mode=WAL`
	// 变为 `?_pragma=foreign_keys(1)&_pragma=journal_mode(WAL)&_pragma=busy_timeout(5000)`。
	// 数据库文件本身格式完全一致，已有 todo.db 可以无缝继续读写。
	dsn := fmt.Sprintf(
		"file:%s?_pragma=foreign_keys(1)&_pragma=journal_mode(WAL)&_pragma=busy_timeout(5000)",
		dbFile,
	)

	gormLogLevel := logger.Warn
	if opts.Verbose {
		gormLogLevel = logger.Info
	}

	// 使用标准 log 包作为输出后端，保留时间戳与前缀，便于与主进程日志对齐。
	stdlog := log.New(os.Stdout, "[gorm] ", log.LstdFlags|log.Lmsgprefix)
	gormLogger := logger.New(
		stdlog,
		logger.Config{
			SlowThreshold:             200 * time.Millisecond,
			LogLevel:                  gormLogLevel,
			IgnoreRecordNotFoundError: true,
			Colorful:                  false,
		},
	)

	db, err := gorm.Open(sqlite.Open(dsn), &gorm.Config{
		Logger: gormLogger,
	})
	if err != nil {
		return nil, fmt.Errorf("open sqlite %q: %w", dsn, err)
	}

	sqlDB, err := db.DB()
	if err != nil {
		return nil, fmt.Errorf("get *sql.DB: %w", err)
	}
	// SQLite 是单写入者数据库，串行化写入避免锁竞争。
	sqlDB.SetMaxOpenConns(1)
	sqlDB.SetMaxIdleConns(1)
	sqlDB.SetConnMaxLifetime(time.Hour)

	if err := db.AutoMigrate(AllModels()...); err != nil {
		return nil, fmt.Errorf("auto migrate: %w", err)
	}
	return db, nil
}
