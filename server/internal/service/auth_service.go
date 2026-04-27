package service

import (
	"crypto/subtle"
	"errors"
	"fmt"
	"time"

	"github.com/golang-jwt/jwt/v5"
)

// ErrInvalidCredentials 登录用户名或密码错误。
var ErrInvalidCredentials = errors.New("invalid username or password")

// AuthService 负责单账号登录校验与 JWT 签发/解析。
// 账号由配置注入（环境变量）；无用户表，所有 token 的 subject 都是该唯一用户名。
type AuthService struct {
	username  string
	password  string
	jwtSecret []byte
	tokenTTL  time.Duration
	// now 可被测试覆盖；生产环境下使用 time.Now。
	now func() time.Time
}

// NewAuthService 构造 AuthService。username/password/jwtSecret 均不允许为空。
func NewAuthService(username, password, jwtSecret string, tokenTTL time.Duration) (*AuthService, error) {
	if username == "" {
		return nil, errors.New("auth: username must not be empty")
	}
	if password == "" {
		return nil, errors.New("auth: password must not be empty")
	}
	if jwtSecret == "" {
		return nil, errors.New("auth: jwtSecret must not be empty")
	}
	if tokenTTL <= 0 {
		return nil, errors.New("auth: tokenTTL must be positive")
	}
	return &AuthService{
		username:  username,
		password:  password,
		jwtSecret: []byte(jwtSecret),
		tokenTTL:  tokenTTL,
		now:       time.Now,
	}, nil
}

// LoginResult 登录成功返回的信息。
type LoginResult struct {
	Token     string    `json:"token"`
	ExpiresAt time.Time `json:"expires_at"`
}

// Login 校验用户名密码，成功返回签发的 JWT。
// 比较使用 constant-time，避免时序攻击。
func (s *AuthService) Login(username, password string) (*LoginResult, error) {
	// 注意：长度不一致时 ConstantTimeCompare 返回 0，也即"不匹配"。
	userOK := subtle.ConstantTimeCompare([]byte(username), []byte(s.username)) == 1
	passOK := subtle.ConstantTimeCompare([]byte(password), []byte(s.password)) == 1
	if !(userOK && passOK) {
		return nil, ErrInvalidCredentials
	}
	return s.issueToken(username)
}

// TokenClaims 自定义 JWT claims。Subject 存 actor 用户名。
type TokenClaims struct {
	jwt.RegisteredClaims
}

// ParseToken 解析并校验 JWT，返回 actor 用户名。
func (s *AuthService) ParseToken(tokenStr string) (string, error) {
	if tokenStr == "" {
		return "", errors.New("auth: empty token")
	}
	parser := jwt.NewParser(
		jwt.WithValidMethods([]string{jwt.SigningMethodHS256.Alg()}),
		jwt.WithIssuedAt(),
	)
	claims := &TokenClaims{}
	tok, err := parser.ParseWithClaims(tokenStr, claims, func(t *jwt.Token) (interface{}, error) {
		return s.jwtSecret, nil
	})
	if err != nil {
		return "", fmt.Errorf("auth: parse token: %w", err)
	}
	if !tok.Valid {
		return "", errors.New("auth: invalid token")
	}
	if claims.Subject == "" {
		return "", errors.New("auth: token missing subject")
	}
	// 只允许签发给配置中的用户名（防止旧密钥复用到不同账号）
	if subtle.ConstantTimeCompare([]byte(claims.Subject), []byte(s.username)) != 1 {
		return "", errors.New("auth: token subject mismatch")
	}
	return claims.Subject, nil
}

// issueToken 内部方法：生成 HS256 JWT。
func (s *AuthService) issueToken(sub string) (*LoginResult, error) {
	now := s.now()
	exp := now.Add(s.tokenTTL)
	claims := TokenClaims{
		RegisteredClaims: jwt.RegisteredClaims{
			Subject:   sub,
			IssuedAt:  jwt.NewNumericDate(now),
			NotBefore: jwt.NewNumericDate(now),
			ExpiresAt: jwt.NewNumericDate(exp),
			Issuer:    "todo-server",
		},
	}
	tok := jwt.NewWithClaims(jwt.SigningMethodHS256, claims)
	signed, err := tok.SignedString(s.jwtSecret)
	if err != nil {
		return nil, fmt.Errorf("auth: sign token: %w", err)
	}
	return &LoginResult{Token: signed, ExpiresAt: exp}, nil
}
