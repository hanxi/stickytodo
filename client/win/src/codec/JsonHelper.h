#pragma once

#include "models/Todo.h"
#include "models/StickyNote.h"
#include "models/Filter.h"
#include "models/AuditLog.h"

#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <optional>

namespace stickytodo::codec {

/// Thin wrapper around nlohmann/json for DTO serialization/deserialization.
/// All parsing is lenient (missing fields get defaults, not exceptions).
class JsonHelper {
public:
    // ---------- Todo ----------
    static models::Todo ParseTodo(const nlohmann::json& j);
    static std::vector<models::Todo> ParseTodos(const std::string& jsonStr);
    static nlohmann::json TodoToJson(const models::Todo& todo);

    // ---------- StickyNote ----------
    static models::StickyNote ParseStickyNote(const nlohmann::json& j);
    static std::vector<models::StickyNote> ParseStickyNotes(const std::string& jsonStr);
    static nlohmann::json StickyNoteToJson(const models::StickyNote& note);

    // ---------- Filter ----------
    static models::Filter ParseFilter(const std::string& jsonStr);
    static std::string FilterToJson(const models::Filter& filter);

    // ---------- AuditLog ----------
    static models::AuditLog ParseAuditLog(const nlohmann::json& j);
    static std::vector<models::AuditLog> ParseAuditLogs(const std::string& jsonStr);

    // ---------- Generic helpers ----------
    static std::string SafeGetString(const nlohmann::json& j, const std::string& key,
                                     const std::string& defaultVal = "");
    static int SafeGetInt(const nlohmann::json& j, const std::string& key, int defaultVal = 0);
    static uint64_t SafeGetUint64(const nlohmann::json& j, const std::string& key, uint64_t defaultVal = 0);
    static bool SafeGetBool(const nlohmann::json& j, const std::string& key, bool defaultVal = false);
    static std::optional<std::string> SafeGetOptionalString(const nlohmann::json& j, const std::string& key);
};

} // namespace stickytodo::codec
