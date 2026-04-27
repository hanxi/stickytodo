#!/usr/bin/env bash
# TODO server end-to-end smoke test.
#
# Prereqs:
#   - Server is already running and reachable at $BASE_URL.
#   - TODO_USERNAME / TODO_PASSWORD match the server's configuration.
#   - curl is installed. jq is NOT required.
#
# Usage:
#   BASE_URL=http://127.0.0.1:8080 \
#   TODO_USERNAME=admin TODO_PASSWORD=test123 \
#     bash scripts/smoke.sh
#
# Exits non-zero on the first failing step.

set -eu
set -o pipefail

BASE_URL="${BASE_URL:-http://127.0.0.1:8080}"
USERNAME="${TODO_USERNAME:-admin}"
PASSWORD="${TODO_PASSWORD:-test123}"

STEP=0
HTTP_CODE=""
HTTP_BODY=""
TOKEN=""

pass() { echo "  [PASS] $*"; }
fail() { echo "  [FAIL] $*" >&2; exit 1; }

step() {
  STEP=$((STEP + 1))
  echo ""
  echo "==> Step ${STEP}: $*"
}

# http_call <METHOD> <PATH> [BODY] [--no-auth]
# Populates HTTP_CODE and HTTP_BODY.
http_call() {
  local method="$1"
  local path="$2"
  local body="${3-}"
  local no_auth="${4-}"

  local tmp
  tmp="$(mktemp)"

  # Build curl args in an array to safely handle empty values under `set -u`.
  local args=(-sS -o "$tmp" -w '%{http_code}' -X "$method" "${BASE_URL}${path}")
  if [[ "$no_auth" != "--no-auth" && -n "${TOKEN}" ]]; then
    args+=(-H "Authorization: Bearer ${TOKEN}")
  fi
  if [[ -n "$body" ]]; then
    args+=(-H "Content-Type: application/json" --data "$body")
  fi

  local code
  if ! code="$(curl "${args[@]}")"; then
    rm -f "$tmp"
    fail "curl failed for $method $path"
  fi
  HTTP_CODE="$code"
  HTTP_BODY="$(cat "$tmp")"
  rm -f "$tmp"
}

expect_code() {
  local want="$1"
  if [[ "$HTTP_CODE" != "$want" ]]; then
    echo "  HTTP ${HTTP_CODE}"
    echo "  BODY: ${HTTP_BODY}"
    fail "expected HTTP ${want}, got ${HTTP_CODE}"
  fi
  pass "HTTP ${want}"
}

expect_contains() {
  local needle="$1"
  if ! printf '%s' "$HTTP_BODY" | grep -qF -- "$needle"; then
    echo "  BODY: ${HTTP_BODY}"
    fail "body does not contain '${needle}'"
  fi
  pass "body contains '${needle}'"
}

# json_string_value <key>
# Extracts a JSON top-level string value (no escape support, sufficient for tokens/titles).
json_string_value() {
  local key="$1"
  printf '%s' "$HTTP_BODY" | sed -n 's/.*"'"$key"'"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' | head -n1
}

# json_number_value <key>
json_number_value() {
  local key="$1"
  printf '%s' "$HTTP_BODY" | sed -n 's/.*"'"$key"'"[[:space:]]*:[[:space:]]*\([0-9][0-9]*\).*/\1/p' | head -n1
}

# ---------------- health ----------------
step "GET /health"
http_call GET /health "" --no-auth
expect_code 200
expect_contains '"status":"ok"'
expect_contains '"server":"todo-server"'

# ---------------- login (bad credentials) ----------------
step "POST /api/login (wrong password -> 401)"
http_call POST /api/login "{\"username\":\"${USERNAME}\",\"password\":\"__definitely_wrong__\"}" --no-auth
expect_code 401
expect_contains '"error"'

# ---------------- login (happy path) ----------------
step "POST /api/login"
http_call POST /api/login "{\"username\":\"${USERNAME}\",\"password\":\"${PASSWORD}\"}" --no-auth
expect_code 200
expect_contains '"token"'
TOKEN="$(json_string_value token)"
[[ -n "$TOKEN" ]] || fail "token missing in login response"
pass "token length=${#TOKEN}"

# ---------------- auth required ----------------
step "GET /api/todos without token -> 401"
SAVED_TOKEN="$TOKEN"; TOKEN=""
http_call GET /api/todos
expect_code 401
TOKEN="$SAVED_TOKEN"

# ---------------- create ----------------
step "POST /api/todos"
http_call POST /api/todos '{"title":"smoke-todo","content":"body","priority":2,"tag":"smoke"}'
expect_code 201
expect_contains '"title":"smoke-todo"'
expect_contains '"status":"pending"'
TODO_ID="$(json_number_value id)"
[[ -n "$TODO_ID" ]] || fail "id missing in create response"
pass "todo id=${TODO_ID}"

# ---------------- list ----------------
step "GET /api/todos"
http_call GET /api/todos
expect_code 200
expect_contains '"items"'
expect_contains '"total"'
expect_contains "\"id\":${TODO_ID}"

step "GET /api/todos?tag=smoke&status=pending"
http_call GET "/api/todos?tag=smoke&status=pending"
expect_code 200
expect_contains "\"id\":${TODO_ID}"

# ---------------- get by id ----------------
step "GET /api/todos/${TODO_ID}"
http_call GET "/api/todos/${TODO_ID}"
expect_code 200
expect_contains '"title":"smoke-todo"'

# ---------------- update ----------------
step "PUT /api/todos/${TODO_ID}"
http_call PUT "/api/todos/${TODO_ID}" '{"title":"smoke-todo-v2","priority":1}'
expect_code 200
expect_contains '"title":"smoke-todo-v2"'
expect_contains '"priority":1'

step "PUT /api/todos/${TODO_ID} (no fields -> 400)"
http_call PUT "/api/todos/${TODO_ID}" '{}'
expect_code 400

step "PUT /api/todos/${TODO_ID} (invalid priority -> 400)"
http_call PUT "/api/todos/${TODO_ID}" '{"priority":99}'
expect_code 400

# ---------------- complete / reopen ----------------
step "POST /api/todos/${TODO_ID}/complete"
http_call POST "/api/todos/${TODO_ID}/complete" ''
expect_code 200
expect_contains '"status":"done"'
expect_contains '"completed_at"'

step "POST /api/todos/${TODO_ID}/reopen"
http_call POST "/api/todos/${TODO_ID}/reopen" ''
expect_code 200
expect_contains '"status":"pending"'

# ---------------- history ----------------
step "GET /api/todos/${TODO_ID}/history"
http_call GET "/api/todos/${TODO_ID}/history"
expect_code 200
expect_contains '"items"'
expect_contains "\"todo_id\":${TODO_ID}"
# Expect at least these actions to be present.
expect_contains '"action":"create"'
expect_contains '"action":"update"'
expect_contains '"action":"complete"'
expect_contains '"action":"reopen"'

# ---------------- tags ----------------
step "GET /api/tags"
http_call GET /api/tags
expect_code 200
expect_contains '"tags"'
expect_contains '"smoke"'

# ---------------- delete (soft) ----------------
step "DELETE /api/todos/${TODO_ID}"
http_call DELETE "/api/todos/${TODO_ID}"
expect_code 200
expect_contains "\"id\":${TODO_ID}"
expect_contains '"deleted":true'

step "GET /api/todos/${TODO_ID} after delete -> 404"
http_call GET "/api/todos/${TODO_ID}"
expect_code 404

step "GET /api/todos/${TODO_ID}?include_deleted=1 after delete -> 200"
http_call GET "/api/todos/${TODO_ID}?include_deleted=1"
expect_code 200
expect_contains "\"id\":${TODO_ID}"

# ---------------- restore ----------------
step "POST /api/todos/${TODO_ID}/restore"
http_call POST "/api/todos/${TODO_ID}/restore" ''
expect_code 200
expect_contains "\"id\":${TODO_ID}"

# ---------------- global audit ----------------
step "GET /api/audit-logs"
http_call GET /api/audit-logs
expect_code 200
expect_contains '"items"'
expect_contains '"action"'
# login success must be audited as well.
expect_contains '"action":"login"'

# ---------------- 404 for unknown ----------------
step "GET /api/todos/9999999 -> 404"
http_call GET /api/todos/9999999
expect_code 404

# ================== Sticky Notes API ==================
#
# 覆盖：list(空) → upsert A → upsert A 改 title(updated_at 变化) → get A →
#       upsert B → list=2 → delete A → get A(404) → 非法 JSON(400) → 未授权(401)
#
# id 由测试脚本生成（UUID 风格的随机字符串），避免依赖系统 uuidgen 以便在最小容器里也能跑。

# 生成稳定的随机 id（小写字母+数字+短横线，满足 service 层 [A-Za-z0-9_-]+ 校验）。
# 格式贴近 UUID 但并不严格遵守 v4 规范——后端只做字符集与长度检查。
gen_sticky_id() {
  local prefix="$1"
  local ts="$(date +%s)"
  local r1="$RANDOM"
  local r2="$RANDOM"
  printf 'smoke-%s-%s-%s-%s' "$prefix" "$ts" "$r1" "$r2"
}

STICKY_A="$(gen_sticky_id a)"
STICKY_B="$(gen_sticky_id b)"

# ---------------- sticky: list empty ----------------
step "GET /api/sticky-notes (before any upsert)"
http_call GET /api/sticky-notes
expect_code 200
expect_contains '"items"'
# 不断言 length==0：历史跑的 smoke 可能已经留下过便签（DB 持久化、不会自动清理）。
# 步骤只验证"端点可用 + 返回 items 字段"。

# ---------------- sticky: upsert A (create) ----------------
step "PUT /api/sticky-notes/${STICKY_A} (create)"
http_call PUT "/api/sticky-notes/${STICKY_A}" "{\"title\":\"sticky-A-v1\",\"frame\":\"{\\\"x\\\":100,\\\"y\\\":100,\\\"width\\\":300,\\\"height\\\":420}\",\"bg_color\":\"{\\\"red\\\":1,\\\"green\\\":0.92,\\\"blue\\\":0.54,\\\"alpha\\\":1}\",\"filter\":\"{\\\"page\\\":1,\\\"page_size\\\":50}\"}"
expect_code 200
expect_contains "\"id\":\"${STICKY_A}\""
expect_contains '"title":"sticky-A-v1"'
STICKY_A_UPDATED_V1="$(json_string_value updated_at)"
[[ -n "$STICKY_A_UPDATED_V1" ]] || fail "updated_at missing in upsert response"
pass "updated_at_v1=${STICKY_A_UPDATED_V1}"

# ---------------- sticky: upsert A again (update) ----------------
# 休眠 1s 确保 updated_at 能在秒级时间戳上拉开差距；避免在快速机器上出现 v1==v2 的伪失败。
sleep 1
step "PUT /api/sticky-notes/${STICKY_A} (title change -> updated_at bumps)"
http_call PUT "/api/sticky-notes/${STICKY_A}" "{\"title\":\"sticky-A-v2\",\"frame\":\"{\\\"x\\\":120,\\\"y\\\":140,\\\"width\\\":300,\\\"height\\\":420}\",\"bg_color\":\"{\\\"red\\\":1,\\\"green\\\":0.92,\\\"blue\\\":0.54,\\\"alpha\\\":1}\",\"filter\":\"{\\\"page\\\":1,\\\"page_size\\\":50}\"}"
expect_code 200
expect_contains '"title":"sticky-A-v2"'
STICKY_A_UPDATED_V2="$(json_string_value updated_at)"
[[ -n "$STICKY_A_UPDATED_V2" ]] || fail "updated_at missing after second upsert"
if [[ "$STICKY_A_UPDATED_V1" == "$STICKY_A_UPDATED_V2" ]]; then
  echo "  updated_at did not change: v1=${STICKY_A_UPDATED_V1} v2=${STICKY_A_UPDATED_V2}"
  fail "OnConflict UPDATE did not bump updated_at (see sticky_repo.go / sticky_service.go)"
fi
pass "updated_at bumped v1=${STICKY_A_UPDATED_V1} -> v2=${STICKY_A_UPDATED_V2}"

# ---------------- sticky: get A ----------------
step "GET /api/sticky-notes/${STICKY_A}"
http_call GET "/api/sticky-notes/${STICKY_A}"
expect_code 200
expect_contains '"title":"sticky-A-v2"'

# ---------------- sticky: upsert B + list contains both ----------------
step "PUT /api/sticky-notes/${STICKY_B} (second sticky)"
http_call PUT "/api/sticky-notes/${STICKY_B}" "{\"title\":\"sticky-B\",\"frame\":\"{\\\"x\\\":500,\\\"y\\\":100,\\\"width\\\":300,\\\"height\\\":420}\",\"bg_color\":\"{\\\"red\\\":0.5,\\\"green\\\":0.5,\\\"blue\\\":1,\\\"alpha\\\":1}\",\"filter\":\"{}\"}"
expect_code 200
expect_contains "\"id\":\"${STICKY_B}\""

step "GET /api/sticky-notes (contains A and B)"
http_call GET /api/sticky-notes
expect_code 200
expect_contains "\"id\":\"${STICKY_A}\""
expect_contains "\"id\":\"${STICKY_B}\""

# ---------------- sticky: delete A (soft) ----------------
step "DELETE /api/sticky-notes/${STICKY_A}"
http_call DELETE "/api/sticky-notes/${STICKY_A}"
expect_code 200
expect_contains "\"id\":\"${STICKY_A}\""
expect_contains '"deleted":true'

step "GET /api/sticky-notes/${STICKY_A} after delete -> 404"
http_call GET "/api/sticky-notes/${STICKY_A}"
expect_code 404

# ---------------- sticky: invalid JSON in filter -> 400 ----------------
# 传入非合法 JSON 的 filter，service 层 normalizeStickyJSON 应当返回 ErrInvalidInput，
# handler 层 writeServiceError 将其映射为 400。
step "PUT /api/sticky-notes/${STICKY_B} (invalid filter JSON -> 400)"
http_call PUT "/api/sticky-notes/${STICKY_B}" "{\"title\":\"sticky-B\",\"frame\":\"{}\",\"bg_color\":\"{}\",\"filter\":\"not-json\"}"
expect_code 400
expect_contains '"error"'

# ---------------- sticky: unauthorized -> 401 ----------------
step "GET /api/sticky-notes without token -> 401"
SAVED_TOKEN="$TOKEN"; TOKEN=""
http_call GET /api/sticky-notes
expect_code 401
TOKEN="$SAVED_TOKEN"

# ---------------- sticky: cleanup (soft delete B) ----------------
# 保持 smoke 幂等：不留脏数据。B 是本轮创建的，删掉它（A 已在上面删过）。
step "DELETE /api/sticky-notes/${STICKY_B} (cleanup)"
http_call DELETE "/api/sticky-notes/${STICKY_B}"
expect_code 200

echo ""
echo "All ${STEP} smoke steps passed."
