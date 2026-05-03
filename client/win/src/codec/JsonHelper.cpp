#include "codec/JsonHelper.h"

namespace stickytodo::codec {

// ---------- Generic helpers ----------

std::string JsonHelper::SafeGetString(const nlohmann::json& j, const std::string& key,
                                       const std::string& defaultVal) {
    if (j.contains(key) && j[key].is_string()) {
        return j[key].get<std::string>();
    }
    return defaultVal;
}

int JsonHelper::SafeGetInt(const nlohmann::json& j, const std::string& key, int defaultVal) {
    if (j.contains(key) && j[key].is_number_integer()) {
        return j[key].get<int>();
    }
    return defaultVal;
}

uint64_t JsonHelper::SafeGetUint64(const nlohmann::json& j, const std::string& key, uint64_t defaultVal) {
    if (j.contains(key) && j[key].is_number_unsigned()) {
        return j[key].get<uint64_t>();
    }
    // Also handle signed integers (backend may return signed)
    if (j.contains(key) && j[key].is_number_integer()) {
        auto val = j[key].get<int64_t>();
        return val >= 0 ? static_cast<uint64_t>(val) : defaultVal;
    }
    return defaultVal;
}

bool JsonHelper::SafeGetBool(const nlohmann::json& j, const std::string& key, bool defaultVal) {
    if (j.contains(key) && j[key].is_boolean()) {
        return j[key].get<bool>();
    }
    return defaultVal;
}

std::optional<std::string> JsonHelper::SafeGetOptionalString(const nlohmann::json& j, const std::string& key) {
    if (j.contains(key)) {
        if (j[key].is_null()) return std::nullopt;
        if (j[key].is_string()) return j[key].get<std::string>();
    }
    return std::nullopt;
}

// ---------- Todo ----------

models::Todo JsonHelper::ParseTodo(const nlohmann::json& j) {
    models::Todo todo;
    todo.id = SafeGetUint64(j, "id");
    todo.title = SafeGetString(j, "title");
    todo.content = SafeGetString(j, "content");
    todo.status = SafeGetString(j, "status", "pending");
    todo.priority = SafeGetInt(j, "priority");
    todo.tag = SafeGetString(j, "tag");
    todo.due_at = SafeGetString(j, "due_at");
    todo.completed_at = SafeGetString(j, "completed_at");
    todo.created_at = SafeGetString(j, "created_at");
    todo.updated_at = SafeGetString(j, "updated_at");
    todo.deleted_at = SafeGetOptionalString(j, "deleted_at");
    return todo;
}

std::vector<models::Todo> JsonHelper::ParseTodos(const std::string& jsonStr) {
    std::vector<models::Todo> result;
    try {
        auto j = nlohmann::json::parse(jsonStr);
        // Backend returns { "items": [...], "total": N, "page": N, "page_size": N }
        nlohmann::json items;
        if (j.is_array()) {
            items = j;
        } else if (j.contains("items") && j["items"].is_array()) {
            items = j["items"];
        } else {
            return result;
        }
        result.reserve(items.size());
        for (const auto& item : items) {
            result.push_back(ParseTodo(item));
        }
    } catch (...) {
        // Return empty on parse error
    }
    return result;
}

nlohmann::json JsonHelper::TodoToJson(const models::Todo& todo) {
    nlohmann::json j;
    if (todo.id > 0) j["id"] = todo.id;
    j["title"] = todo.title;
    if (!todo.content.empty()) j["content"] = todo.content;
    j["status"] = todo.status;
    j["priority"] = todo.priority;
    if (!todo.tag.empty()) j["tag"] = todo.tag;
    if (!todo.due_at.empty()) j["due_at"] = todo.due_at;
    return j;
}

// ---------- StickyNote ----------

models::StickyNote JsonHelper::ParseStickyNote(const nlohmann::json& j) {
    models::StickyNote note;
    note.id = SafeGetString(j, "id");
    note.title = SafeGetString(j, "title");
    note.bg_color = SafeGetString(j, "bg_color", "{}");
    note.filter = SafeGetString(j, "filter", "{}");
    note.frame = SafeGetString(j, "frame", "{}");
    note.created_at = SafeGetString(j, "created_at");
    note.updated_at = SafeGetString(j, "updated_at");
    return note;
}

std::vector<models::StickyNote> JsonHelper::ParseStickyNotes(const std::string& jsonStr) {
    std::vector<models::StickyNote> result;
    try {
        auto j = nlohmann::json::parse(jsonStr);
        // Backend returns { "items": [...] }
        nlohmann::json items;
        if (j.is_array()) {
            items = j;
        } else if (j.contains("items") && j["items"].is_array()) {
            items = j["items"];
        } else {
            return result;
        }
        result.reserve(items.size());
        for (const auto& item : items) {
            result.push_back(ParseStickyNote(item));
        }
    } catch (...) {
        // Return empty on parse error
    }
    return result;
}

nlohmann::json JsonHelper::StickyNoteToJson(const models::StickyNote& note) {
    nlohmann::json j;
    j["title"] = note.title;
    j["frame"] = note.frame.empty() ? "{}" : note.frame;
    j["bg_color"] = note.bg_color.empty() ? "{}" : note.bg_color;
    j["filter"] = note.filter.empty() ? "{}" : note.filter;
    return j;
}

// ---------- Filter ----------

models::Filter JsonHelper::ParseFilter(const std::string& jsonStr) {
    models::Filter filter;
    if (jsonStr.empty() || jsonStr == "{}") return filter;

    try {
        auto j = nlohmann::json::parse(jsonStr);
        filter.status = SafeGetString(j, "status");
        filter.tag = SafeGetString(j, "tag");
        filter.keyword = SafeGetString(j, "keyword");
        filter.due_before = SafeGetString(j, "due_before");
        filter.include_deleted = SafeGetBool(j, "include_deleted");
        filter.only_deleted = SafeGetBool(j, "only_deleted");
        filter.page = SafeGetInt(j, "page", 1);
        filter.page_size = SafeGetInt(j, "page_size", 50);
    } catch (...) {
        // Return default filter
    }
    return filter;
}

std::string JsonHelper::FilterToJson(const models::Filter& filter) {
    nlohmann::json j;
    if (!filter.status.empty()) j["status"] = filter.status;
    if (!filter.tag.empty()) j["tag"] = filter.tag;
    if (!filter.keyword.empty()) j["keyword"] = filter.keyword;
    if (!filter.due_before.empty()) j["due_before"] = filter.due_before;
    if (filter.include_deleted) j["include_deleted"] = true;
    if (filter.only_deleted) j["only_deleted"] = true;
    if (filter.page > 1) j["page"] = filter.page;
    if (filter.page_size != 50) j["page_size"] = filter.page_size;
    return j.dump();
}

// ---------- AuditLog ----------

models::AuditLog JsonHelper::ParseAuditLog(const nlohmann::json& j) {
    models::AuditLog log;
    log.id = SafeGetUint64(j, "id");
    auto todoId = SafeGetOptionalString(j, "todo_id");
    if (j.contains("todo_id") && j["todo_id"].is_number()) {
        log.todo_id = j["todo_id"].get<uint64_t>();
    }
    log.action = SafeGetString(j, "action");
    log.detail = SafeGetString(j, "detail");
    log.actor = SafeGetString(j, "actor");
    log.ip = SafeGetString(j, "ip");
    log.user_agent = SafeGetString(j, "user_agent");
    log.created_at = SafeGetString(j, "created_at");
    return log;
}

std::vector<models::AuditLog> JsonHelper::ParseAuditLogs(const std::string& jsonStr) {
    std::vector<models::AuditLog> result;
    try {
        auto j = nlohmann::json::parse(jsonStr);
        nlohmann::json items;
        if (j.is_array()) {
            items = j;
        } else if (j.contains("items") && j["items"].is_array()) {
            items = j["items"];
        } else {
            return result;
        }
        result.reserve(items.size());
        for (const auto& item : items) {
            result.push_back(ParseAuditLog(item));
        }
    } catch (...) {
        // Return empty on parse error
    }
    return result;
}

} // namespace stickytodo::codec
