#pragma once

#include <string>

namespace stickytodo::models {

/// Filter criteria for TODO display within a sticky note.
/// Aligned with backend query parameters and macOS TodoFilterDTO (snake_case JSON).
///
/// Backend query params: status, tag, keyword, due_before, include_deleted,
///                       only_deleted, page, page_size
/// The filter is stored as a JSON string in StickyNote.filter field.
struct Filter {
    std::string status;             // "" | "pending" | "done" | "all"
    std::string tag;                // single tag name; empty = all
    std::string keyword;            // search text in title/content
    std::string due_before;         // RFC3339 datetime string or empty
    bool include_deleted = false;   // include soft-deleted items
    bool only_deleted = false;      // show only deleted items
    int page = 1;                   // pagination page number
    int page_size = 50;             // items per page

    /// Returns true if no meaningful filter criteria are set.
    bool IsEmpty() const {
        return status.empty()
            && tag.empty()
            && keyword.empty()
            && due_before.empty()
            && !include_deleted
            && !only_deleted;
    }
};

} // namespace stickytodo::models
