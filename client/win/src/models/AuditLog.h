#pragma once

#include <string>
#include <cstdint>
#include <optional>

namespace stickytodo::models {

/// Audit log entry DTO — aligned with backend model (server/internal/model/models.go).
/// JSON tags are snake_case.
struct AuditLog {
    uint64_t id = 0;
    std::optional<uint64_t> todo_id;  // nullable; links to related todo
    std::string action;               // e.g. "todo.created", "todo.updated", "todo.deleted"
    std::string detail;               // JSON string with change details
    std::string actor;                // username who performed the action
    std::string ip;                   // client IP address
    std::string user_agent;           // client user agent string
    std::string created_at;           // ISO 8601 datetime
};

} // namespace stickytodo::models
