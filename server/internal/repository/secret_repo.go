// Package repository 提供对 DB 的读写抽象。本文件实现 app_secrets 表的读写，
// 目前主要服务于 JWT 签名密钥的"启动时生成 + 持久化 + 后续复用"。
package repository

import (
	"crypto/rand"
	"encoding/hex"
	"errors"
	"fmt"
	"strings"

	"gorm.io/gorm"

	"github.com/hanxi/todo-server/internal/model"
)

// secretKeyJWT 是 app_secrets 表中 JWT 签名密钥的固定 key。
const secretKeyJWT = "jwt_secret"

// jwtSecretBytes 是 JWT 签名密钥的字节长度（256 bit，HS256 的最小推荐长度）。
const jwtSecretBytes = 32

// GetOrCreateJWTSecret 读取并返回当前 DB 中保存的 JWT 签名密钥；
// 若不存在，则生成 32 字节随机值（hex 编码 64 字符）写入并返回。
//
// 并发语义：SQLite 单写入者 + 主键冲突检测；两个进程同时首次启动时，
// 晚写入的一方会拿到 ErrDuplicatedKey，此时重新 Select 即可。
// 本函数用一次"先读→读不到就写→写失败再读"三步兜底完成幂等。
func GetOrCreateJWTSecret(db *gorm.DB) (string, error) {
	if db == nil {
		return "", errors.New("secret_repo: db is nil")
	}

	// 1) 读
	if secret, ok, err := loadJWTSecret(db); err != nil {
		return "", fmt.Errorf("secret_repo: load: %w", err)
	} else if ok {
		return secret, nil
	}

	// 2) 生成
	secret, err := randomHex(jwtSecretBytes)
	if err != nil {
		return "", fmt.Errorf("secret_repo: generate: %w", err)
	}

	// 3) 尝试写入。若并发下另一进程已先写，这里会报主键冲突；
	//    此时重新 Select 取对方写入的值，保证同一 DB 最终只有一个 secret。
	row := model.AppSecret{Key: secretKeyJWT, Value: secret}
	if createErr := db.Create(&row).Error; createErr != nil {
		if isDuplicateKeyErr(createErr) {
			if existing, ok, loadErr := loadJWTSecret(db); loadErr != nil {
				return "", fmt.Errorf("secret_repo: reload after conflict: %w", loadErr)
			} else if ok {
				return existing, nil
			}
			return "", fmt.Errorf("secret_repo: duplicate key but row missing: %w", createErr)
		}
		return "", fmt.Errorf("secret_repo: create: %w", createErr)
	}
	return secret, nil
}

// loadJWTSecret 从 DB 读取当前 JWT 密钥。
// ok=false 表示表里还不存在该 key；err 仅返回真正的 IO/查询错误。
func loadJWTSecret(db *gorm.DB) (string, bool, error) {
	var row model.AppSecret
	err := db.Where("key = ?", secretKeyJWT).Take(&row).Error
	if err != nil {
		if errors.Is(err, gorm.ErrRecordNotFound) {
			return "", false, nil
		}
		return "", false, err
	}
	// 防御性校验：若历史数据损坏（空串或长度异常），视为"不存在"，由上层重新生成。
	// 有效值必须是 2*jwtSecretBytes = 64 个字符的 hex。
	if len(row.Value) != jwtSecretBytes*2 || !isHex(row.Value) {
		return "", false, nil
	}
	return row.Value, true, nil
}

// isDuplicateKeyErr 判断 GORM 返回的错误是否为主键/唯一约束冲突。
// GORM v1.31+ 定义了 ErrDuplicatedKey；为了兼容驱动层返回的错误文本
// （SQLite 是 "UNIQUE constraint failed: ..."），这里同时做字符串匹配兜底。
func isDuplicateKeyErr(err error) bool {
	if errors.Is(err, gorm.ErrDuplicatedKey) {
		return true
	}
	msg := strings.ToLower(err.Error())
	return strings.Contains(msg, "unique constraint failed") ||
		strings.Contains(msg, "duplicate")
}

// isHex 判断字符串是否全部由 16 进制字符组成。
func isHex(s string) bool {
	for _, c := range s {
		switch {
		case c >= '0' && c <= '9':
		case c >= 'a' && c <= 'f':
		case c >= 'A' && c <= 'F':
		default:
			return false
		}
	}
	return len(s) > 0
}

// randomHex 生成 n 字节加密安全随机值的十六进制编码字符串（返回长度 = 2n）。
func randomHex(n int) (string, error) {
	buf := make([]byte, n)
	if _, err := rand.Read(buf); err != nil {
		return "", err
	}
	return hex.EncodeToString(buf), nil
}
