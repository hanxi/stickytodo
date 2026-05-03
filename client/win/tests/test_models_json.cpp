// test_models_json.cpp — JsonHelper DTO parsing + serialisation tests.
//
// Scope: the per-DTO helpers in codec/JsonHelper.cpp (Todo / StickyNote /
// AuditLog). These helpers guard the REST wire format so that schema drift
// between the Go backend and the Windows client shows up here instead of
// as a crashing `JSONDecoder.decode(...)` call in production.
//
// Input fixtures are deliberately close to real server responses: same
// field names, same casing, same value shapes (uint IDs, RFC3339 dates,
// optional nullable deleted_at). Anything that compiles without a field
// present must still decode gracefully — the helpers document themselves
// as "lenient, fill in defaults on missing/malformed".

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "codec/JsonHelper.h"
#include "models/Todo.h"
#include "models/StickyNote.h"
#include "models/AuditLog.h"
#include "models/Filter.h"

#include <string>

using stickytodo::codec::JsonHelper;
using stickytodo::models::Todo;
using stickytodo::models::StickyNote;
using stickytodo::models::AuditLog;
using stickytodo::models::Filter;

// =============== Todo ==========================================================

TEST(JsonHelper_ParseTodo, AllFieldsRoundTripFromFullServerShape) {
    auto j = nlohmann::json::parse(R"({
        "id": 42,
        "title": "Ship release",
        "content": "zip, sign, upload",
        "status": "pending",
        "priority": 2,
        "tag": "release",
        "due_at": "2025-11-01T09:00:00Z",
        "completed_at": "",
        "created_at": "2025-10-20T03:00:00Z",
        "updated_at": "2025-10-21T03:00:00Z",
        "deleted_at": null
    })");
    Todo t = JsonHelper::ParseTodo(j);
    EXPECT_EQ(t.id, 42u);
    EXPECT_EQ(t.title, "Ship release");
    EXPECT_EQ(t.content, "zip, sign, upload");
    EXPECT_EQ(t.status, "pending");
    EXPECT_EQ(t.priority, 2);
    EXPECT_EQ(t.tag, "release");
    EXPECT_EQ(t.due_at, "2025-11-01T09:00:00Z");
    EXPECT_EQ(t.completed_at, "");
    EXPECT_FALSE(t.deleted_at.has_value());
    EXPECT_FALSE(t.IsDeleted());
    EXPECT_TRUE(t.IsPending());
}

TEST(JsonHelper_ParseTodo, MissingFieldsGetDefaultsNotErrors) {
    // Server bug, old schema, or a manually-truncated payload must not
    // crash — the client is lenient and fills defaults.
    auto j = nlohmann::json::parse(R"({"title":"orphan"})");
    Todo t = JsonHelper::ParseTodo(j);
    EXPECT_EQ(t.id, 0u);
    EXPECT_EQ(t.title, "orphan");
    EXPECT_EQ(t.status, "pending");   // default
    EXPECT_EQ(t.priority, 0);
    EXPECT_FALSE(t.deleted_at.has_value());
}

TEST(JsonHelper_ParseTodo, DeletedAtPresentMarksTodoDeleted) {
    auto j = nlohmann::json::parse(R"({
        "id": 7, "title":"x", "status":"done",
        "deleted_at":"2025-10-22T00:00:00Z"
    })");
    Todo t = JsonHelper::ParseTodo(j);
    EXPECT_TRUE(t.IsDeleted());
    ASSERT_TRUE(t.deleted_at.has_value());
    EXPECT_EQ(*t.deleted_at, "2025-10-22T00:00:00Z");
    EXPECT_TRUE(t.IsDone());
}

TEST(JsonHelper_ParseTodos, AcceptsBareArray) {
    std::string src = R"([{"id":1,"title":"a"},{"id":2,"title":"b"}])";
    auto v = JsonHelper::ParseTodos(src);
    ASSERT_EQ(v.size(), 2u);
    EXPECT_EQ(v[0].id, 1u);
    EXPECT_EQ(v[1].title, "b");
}

TEST(JsonHelper_ParseTodos, AcceptsPaginatedEnvelope) {
    // Backend: `{"items":[...], "total":N, "page":N, "page_size":N}`.
    // The client only cares about the array — total/page are rehydrated
    // separately.
    std::string src = R"({
        "items":[{"id":10,"title":"x"}],
        "total":1, "page":1, "page_size":50
    })";
    auto v = JsonHelper::ParseTodos(src);
    ASSERT_EQ(v.size(), 1u);
    EXPECT_EQ(v[0].id, 10u);
}

TEST(JsonHelper_ParseTodos, MalformedJsonYieldsEmptyVector) {
    auto v = JsonHelper::ParseTodos("{this is not json");
    EXPECT_TRUE(v.empty());
}

TEST(JsonHelper_TodoToJson, EmitsOnlyMeaningfulFields) {
    // We don't round-trip id/timestamps back to the server — POST /api/todos
    // doesn't accept them. Verify the encoder strips them.
    Todo t;
    t.id = 5;
    t.title = "new";
    t.status = "pending";
    t.priority = 3;
    t.tag = "bug";
    t.due_at = "2025-12-01T00:00:00Z";
    t.content = "";              // default — should be omitted

    auto j = JsonHelper::TodoToJson(t);
    EXPECT_TRUE(j.contains("id"));       // id>0 emitted (used for PUT paths)
    EXPECT_EQ(j["title"], "new");
    EXPECT_EQ(j["status"], "pending");
    EXPECT_EQ(j["priority"], 3);
    EXPECT_EQ(j["tag"], "bug");
    EXPECT_EQ(j["due_at"], "2025-12-01T00:00:00Z");
    EXPECT_FALSE(j.contains("content"));
    EXPECT_FALSE(j.contains("created_at"));  // server-owned field
    EXPECT_FALSE(j.contains("deleted_at"));
}

TEST(JsonHelper_TodoToJson, ZeroIdIsOmitted) {
    Todo t; t.title = "x"; t.status = "pending";
    auto j = JsonHelper::TodoToJson(t);
    EXPECT_FALSE(j.contains("id")) << "id=0 must be stripped so POST creates a new row";
}

// =============== StickyNote ====================================================

TEST(JsonHelper_ParseStickyNote, AcceptsFullServerShape) {
    auto j = nlohmann::json::parse(R"({
        "id":"3c1bff72-93f1-4a4d-b7a8-aa90ef9b6e3a",
        "title":"Inbox",
        "bg_color":"{\"red\":1.0,\"green\":0.92,\"blue\":0.54,\"alpha\":1.0}",
        "filter":"{\"status\":\"pending\"}",
        "frame":"{}",
        "created_at":"2025-10-20T03:00:00Z",
        "updated_at":"2025-10-21T03:00:00Z"
    })");
    StickyNote n = JsonHelper::ParseStickyNote(j);
    EXPECT_EQ(n.id, "3c1bff72-93f1-4a4d-b7a8-aa90ef9b6e3a");
    EXPECT_EQ(n.title, "Inbox");
    EXPECT_FALSE(n.bg_color.empty());
    EXPECT_FALSE(n.filter.empty());
    EXPECT_EQ(n.frame, "{}");
}

TEST(JsonHelper_ParseStickyNote, MissingNestedJsonStringsDefaultToEmptyObject) {
    // AGENTS.md §3.4: bg_color / filter / frame are opaque JSON strings
    // and must always be present (at least as "{}"). If the server or
    // some other client omits them, the Windows side must fill in "{}"
    // so decoders downstream don't have to handle the empty case.
    auto j = nlohmann::json::parse(R"({"id":"abc","title":"Empty"})");
    StickyNote n = JsonHelper::ParseStickyNote(j);
    EXPECT_EQ(n.bg_color, "{}");
    EXPECT_EQ(n.filter, "{}");
    EXPECT_EQ(n.frame, "{}");
}

TEST(JsonHelper_ParseStickyNotes, AcceptsItemsEnvelope) {
    std::string src = R"({
        "items":[
            {"id":"a","title":"A"},
            {"id":"b","title":"B","filter":"{}"}
        ]
    })";
    auto v = JsonHelper::ParseStickyNotes(src);
    ASSERT_EQ(v.size(), 2u);
    EXPECT_EQ(v[0].id, "a");
    EXPECT_EQ(v[1].title, "B");
}

TEST(JsonHelper_StickyNoteToJson, FrameIsAlwaysPersistedAsEmptyObject) {
    // AGENTS.md §3.4 contract: Windows (like Web) does NOT use the server's
    // frame field — window position lives in FrameStore locally. The
    // upsert payload must still serialise frame as a valid JSON object so
    // the backend's `json.Valid(frame)` check passes.
    StickyNote n;
    n.title = "Position-less";
    n.bg_color = R"({"red":1,"green":1,"blue":1,"alpha":1})";
    n.filter = "{}";
    n.frame = "";  // whatever the caller set — must be normalised

    auto j = JsonHelper::StickyNoteToJson(n);
    EXPECT_EQ(j["frame"], "{}");
    EXPECT_EQ(j["title"], "Position-less");
    // id / created_at / updated_at are server-assigned; client upsert
    // payload must not echo them back.
    EXPECT_FALSE(j.contains("id"));
    EXPECT_FALSE(j.contains("created_at"));
    EXPECT_FALSE(j.contains("updated_at"));
}

TEST(JsonHelper_StickyNoteToJson, EmptyBgColorAndFilterNormalisedToEmptyObject) {
    StickyNote n; n.title = "blank";
    auto j = JsonHelper::StickyNoteToJson(n);
    EXPECT_EQ(j["bg_color"], "{}");
    EXPECT_EQ(j["filter"], "{}");
    EXPECT_EQ(j["frame"], "{}");
}

// =============== AuditLog ======================================================

TEST(JsonHelper_ParseAuditLog, FullShape) {
    auto j = nlohmann::json::parse(R"({
        "id": 101,
        "todo_id": 42,
        "action": "todo.updated",
        "detail": "{\"title\":\"old->new\"}",
        "actor": "admin",
        "ip": "127.0.0.1",
        "user_agent": "stickytodo-win/1.0",
        "created_at": "2025-10-21T03:00:01Z"
    })");
    AuditLog a = JsonHelper::ParseAuditLog(j);
    EXPECT_EQ(a.id, 101u);
    ASSERT_TRUE(a.todo_id.has_value());
    EXPECT_EQ(*a.todo_id, 42u);
    EXPECT_EQ(a.action, "todo.updated");
    EXPECT_EQ(a.detail, R"({"title":"old->new"})");
    EXPECT_EQ(a.actor, "admin");
    EXPECT_EQ(a.ip, "127.0.0.1");
    EXPECT_EQ(a.user_agent, "stickytodo-win/1.0");
    EXPECT_EQ(a.created_at, "2025-10-21T03:00:01Z");
}

TEST(JsonHelper_ParseAuditLog, TodoIdMayBeAbsentForStickyEvents) {
    // sticky.* audit entries carry no todo_id. Ensure the optional is
    // empty rather than zero — consumers rely on .has_value() to
    // distinguish "genuinely zero" from "sticky-scoped".
    auto j = nlohmann::json::parse(R"({
        "id":5, "action":"sticky.upserted", "actor":"admin"
    })");
    AuditLog a = JsonHelper::ParseAuditLog(j);
    EXPECT_FALSE(a.todo_id.has_value());
    EXPECT_EQ(a.action, "sticky.upserted");
}

TEST(JsonHelper_ParseAuditLogs, AcceptsPaginatedEnvelope) {
    std::string src = R"({
        "items":[
            {"id":1,"action":"login","actor":"admin"},
            {"id":2,"action":"todo.created","todo_id":9,"actor":"admin"}
        ],
        "total":2
    })";
    auto v = JsonHelper::ParseAuditLogs(src);
    ASSERT_EQ(v.size(), 2u);
    EXPECT_EQ(v[0].action, "login");
    EXPECT_TRUE(v[1].todo_id.has_value());
}

// =============== Filter via JsonHelper (parallel path) =========================
//
// JsonHelper::ParseFilter / FilterToJson sit alongside StickyCodec's filter
// helpers. Both must behave identically so whichever decoder is reached
// first yields the same Filter — a narrow but critical invariant because
// different call sites (REST pagination vs sticky field) enter through
// different helpers.

TEST(JsonHelper_ParseFilter, RoundTripsWithFilterToJson) {
    Filter src;
    src.status = "done";
    src.tag = "home";
    src.page_size = 25;
    std::string json = JsonHelper::FilterToJson(src);
    Filter dst = JsonHelper::ParseFilter(json);
    EXPECT_EQ(dst.status, "done");
    EXPECT_EQ(dst.tag, "home");
    EXPECT_EQ(dst.page_size, 25);
}

// =============== Generic helpers ==============================================

TEST(JsonHelper_SafeGetUint64, AcceptsBothSignedAndUnsigned) {
    auto j = nlohmann::json::parse(R"({"a": 42, "b": -1, "c": "nope"})");
    EXPECT_EQ(JsonHelper::SafeGetUint64(j, "a"), 42u);
    // Negative: fall back to default rather than returning a huge wraparound.
    EXPECT_EQ(JsonHelper::SafeGetUint64(j, "b", 99u), 99u);
    // Wrong type: default.
    EXPECT_EQ(JsonHelper::SafeGetUint64(j, "c", 7u), 7u);
    // Missing: default.
    EXPECT_EQ(JsonHelper::SafeGetUint64(j, "missing", 0u), 0u);
}

TEST(JsonHelper_SafeGetOptionalString, DistinguishesNullFromMissing) {
    auto j = nlohmann::json::parse(R"({"a":null, "b":"x"})");
    // null → nullopt (explicit erase)
    EXPECT_FALSE(JsonHelper::SafeGetOptionalString(j, "a").has_value());
    // present → value
    auto b = JsonHelper::SafeGetOptionalString(j, "b");
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(*b, "x");
    // missing → nullopt (no exception)
    EXPECT_FALSE(JsonHelper::SafeGetOptionalString(j, "c").has_value());
}
