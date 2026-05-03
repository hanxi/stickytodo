#pragma once

#include <string>
#include <optional>
#include <cstdint>

namespace stickytodo::models {

/// Todo item DTO — aligned with backend JSON schema (snake_case JSON tags).
/// Backend model: server/internal/model/models.go → Todo struct.
struct Todo {
    uint64_t id = 0;
    std::string title;
    std::string content;        // text body (may be empty)
    std::string status;         // "pending" | "done"
    int priority = 0;           // 0-3 (0=none, 1=low, 2=medium, 3=high)
    std::string tag;            // single tag string (backend field is "tag", not "tags")
    std::string due_at;         // ISO 8601 datetime or empty (backend: "due_at")
    std::string completed_at;   // ISO 8601 datetime or empty
    std::string created_at;     // ISO 8601 datetime
    std::string updated_at;     // ISO 8601 datetime
    std::optional<std::string> deleted_at; // null/omitted if not soft-deleted

    bool IsPending() const { return status == "pending"; }
    bool IsDone() const { return status == "done"; }
    bool IsDeleted() const { return deleted_at.has_value(); }
};

} // namespace stickytodo::models
