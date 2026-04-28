#!/usr/bin/env bash
# TODO server end-to-end smoke test.
#
# Prereqs:
#   - Server is already running and reachable at $BASE_URL.
#   - TODO_USERNAME / TODO_PASSWORD match the server's configuration.
#   - curl is installed. jq is NOT required.
#   - go toolchain is installed (used to build ws-probe for Step 33-36).
#     The build happens once at script start; set WS_PROBE_BIN to an existing
#     binary to reuse it across runs.
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

# ---------- ws-probe 构建（用于 Step 33-36 的 WebSocket 回归） ----------
# 允许外部注入 WS_PROBE_BIN 以便复用；默认每次跑 smoke 时单独构建到临时目录。
SMOKE_SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SERVER_DIR="$(cd "${SMOKE_SCRIPT_DIR}/.." && pwd)"
if [[ -z "${WS_PROBE_BIN:-}" ]]; then
  WS_PROBE_BIN="$(mktemp -d)/ws-probe"
fi
echo "==> Building ws-probe -> ${WS_PROBE_BIN}"
if ! (cd "${SERVER_DIR}" && go build -o "${WS_PROBE_BIN}" ./scripts/ws-probe); then
  echo "  [FAIL] ws-probe build failed" >&2
  exit 1
fi
# 从 BASE_URL 推导 ws:// / wss:// 的 WebSocket URL。
# 先处理 https → wss，再处理 http → ws：bash 参数展开按字符串前缀匹配而非正则，
# 严格意义上 "https:" 里不含子串 "http:"（中间是 's'），两种顺序当前都正确；
# 但先替换更长的前缀是更防御性的写法，未来无论 BASE_URL 是否被扩展都不会误伤。
WS_URL="${BASE_URL/https:/wss:}"
WS_URL="${WS_URL/http:/ws:}"
WS_URL="${WS_URL%/}/api/ws"
echo "  [INFO] WS_URL=${WS_URL}"

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

# ================== WebSocket / 实时推送 ==================
#
# 覆盖四个场景（与 ws-probe 的四种模式一一对应）：
#   Step 33: 连上不发 auth → 服务端在 authTimeout(=2s) 内以 4401 关闭
#   Step 34: 发送 token=BAD 的 auth 帧 → 服务端以 4401 关闭
#   Step 35: 用有效 token auth → 收到 {"type":"ready"}
#   Step 36: auth 成功后后台阻塞等待 todo.created 事件，主线程 POST /api/todos 触发推送
#
# ws-probe 退出码：0=通过；1=断言失败；2=参数错误。
# 这里的 -timeout 值都给出充分余量（server 侧 authTimeout=2s，我们给 4s）。

# ---------------- ws: Step 33 未发 auth → 4401 ----------------
step "WS /api/ws: no auth frame within 2s -> close 4401"
if ! "${WS_PROBE_BIN}" -mode no-auth -url "${WS_URL}" -expect-code 4401 -timeout 4s; then
  fail "ws-probe no-auth failed"
fi
pass "ws close 4401 on missing auth"

# ---------------- ws: Step 34 bad token → 4401 ----------------
# ws-probe 的 bad-token 模式要求 -token 非空（用作占位，实际会被 server 拒）
step "WS /api/ws: auth with invalid token -> close 4401"
if ! "${WS_PROBE_BIN}" -mode bad-token -url "${WS_URL}" -token "BAD.TOKEN.VALUE" -expect-code 4401 -timeout 4s; then
  fail "ws-probe bad-token failed"
fi
pass "ws close 4401 on invalid token"

# ---------------- ws: Step 35 auth ready ----------------
step "WS /api/ws: auth with valid token -> receive ready frame"
if ! "${WS_PROBE_BIN}" -mode auth-ready -url "${WS_URL}" -token "${TOKEN}" -timeout 4s; then
  fail "ws-probe auth-ready failed"
fi
pass "ws ready frame received"

# ---------------- ws: Step 36 live event push (单 step 内部做 probe + 触发 + 清理) ----------------
# 本步合并三件事，使整个 WS 章节保持 4 个 step（33/34/35/36）与计划一致：
#  1) 后台启动 ws-probe wait-event，stdout 接到 FIFO，stderr 接到日志文件；
#     probe 在完成 dial + 发 auth + 吞掉 {"type":"ready"} 之后，会往 stdout 写一行 "READY"
#  2) 主线程阻塞从 FIFO 读一行（带 5s 超时），看到 "READY" 才继续——彻底消除
#     "sleep N 秒等 probe 握手完成" 的经验时序假设
#  3) 主线程 POST /api/todos：TodoService.Create 成功后会同步调用
#     broadcaster.BroadcastTodoCreated → Hub.Broadcast 扇出到所有已注册客户端
#  4) wait 读 probe 退出码：0=匹配到事件；非 0=失败（打印日志并 fail）
#  5) 把步骤 3 创建的 todo 软删掉，保持 smoke 幂等
#
# `wait $PID` 在 `set -e` 下子进程非 0 会让脚本直接退出；这里临时关 set -e 抓退出码。
step "WS /api/ws: live push 'todo.created' after REST create + cleanup"
WS_PROBE_TMPDIR="$(mktemp -d)"
WS_PROBE_LOG="${WS_PROBE_TMPDIR}/ws-probe.stderr.log"
WS_PROBE_FIFO="${WS_PROBE_TMPDIR}/ws-probe.ready.fifo"
mkfifo "${WS_PROBE_FIFO}" || fail "mkfifo failed: ${WS_PROBE_FIFO}"

# 后台启动 probe：stdout → FIFO（用于 READY 信号），stderr → log 文件（用于失败诊断）
"${WS_PROBE_BIN}" -mode wait-event \
  -url "${WS_URL}" \
  -token "${TOKEN}" \
  -expect-type todo.created \
  -timeout 8s \
  >"${WS_PROBE_FIFO}" 2>"${WS_PROBE_LOG}" &
WS_PID=$!

# ws_cleanup 统一清理后台 probe + 临时文件。需要幂等，在多个失败分支被复用。
ws_cleanup() {
  if [[ -n "${WS_PID:-}" ]] && kill -0 "${WS_PID}" 2>/dev/null; then
    kill "${WS_PID}" 2>/dev/null || true
    wait "${WS_PID}" 2>/dev/null || true
  fi
  rm -rf "${WS_PROBE_TMPDIR}"
}

# 带超时从 FIFO 读一行 READY 信号。
# read -t 5：给 probe 最多 5s 完成握手（本地回环应 << 100ms，5s 是极宽松保底）；
# 打开 FIFO 用 exec 3< ... 的写法避免"每次 read 都重新打开 FIFO"的阻塞语义；
# IFS= 保持原始内容，-r 禁止反斜杠转义。
set +e
exec 3<"${WS_PROBE_FIFO}"
read -r -t 5 -u 3 WS_READY_LINE
READ_EXIT=$?
exec 3<&-
set -e
if [[ "${READ_EXIT}" -ne 0 ]] || [[ "${WS_READY_LINE}" != "READY" ]]; then
  echo "  read-ready exit=${READ_EXIT}, got line: '${WS_READY_LINE:-<empty>}'"
  echo "  ws-probe stderr:"
  sed 's/^/    /' "${WS_PROBE_LOG}" >&2 || true
  ws_cleanup
  fail "ws-probe did not emit READY signal within 5s (handshake failed?)"
fi

# 触发事件：创建一条 todo（service 层成功后会广播 todo.created）
http_call POST /api/todos '{"title":"smoke-ws-trigger","priority":3,"tag":"smoke-ws"}'
if [[ "$HTTP_CODE" != "201" ]]; then
  # POST 失败：杀 probe + 清理临时目录
  ws_cleanup
  echo "  BODY: ${HTTP_BODY}"
  fail "POST /api/todos expected 201, got ${HTTP_CODE}"
fi
WS_TRIGGER_TODO_ID="$(json_number_value id)"
if [[ -z "$WS_TRIGGER_TODO_ID" ]]; then
  ws_cleanup
  fail "trigger todo id missing in POST response"
fi

# 等待后台 probe 退出，读取退出码。
set +e
wait "${WS_PID}"
WS_EXIT=$?
set -e
if [[ "${WS_EXIT}" -ne 0 ]]; then
  echo "  ws-probe stderr:"
  sed 's/^/    /' "${WS_PROBE_LOG}" >&2 || true
  # 幂等兜底：WS 断言失败时仍要尽力删掉刚创建的 todo，避免留脏数据。
  # 不能走 http_call——它在 curl 失败时会调 fail→exit，而且成功但状态码不对时
  # 也不会返回错误码；这里我们**不关心**能否删成功，所以直接走 curl，吞掉所有退出码。
  curl -sS -o /dev/null \
    -X DELETE "${BASE_URL}/api/todos/${WS_TRIGGER_TODO_ID}" \
    -H "Authorization: Bearer ${TOKEN}" 2>/dev/null || true
  rm -rf "${WS_PROBE_TMPDIR}"
  fail "ws-probe wait-event exited with code ${WS_EXIT} (expected 0; want todo.created within 8s)"
fi
rm -rf "${WS_PROBE_TMPDIR}"
pass "ws received todo.created within 8s of POST (trigger todo id=${WS_TRIGGER_TODO_ID})"

# 步骤内 cleanup：软删触发用的 todo，保持 smoke 幂等（不新增 step）
http_call DELETE "/api/todos/${WS_TRIGGER_TODO_ID}"
expect_code 200

echo ""
echo "All ${STEP} smoke steps passed."
