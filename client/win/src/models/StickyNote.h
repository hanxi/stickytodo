#pragma once

#include <string>

namespace stickytodo::models {

/// Sticky note DTO — aligned with backend JSON schema.
struct StickyNote {
    std::string id;             // UUID string
    std::string title;          // Display title
    std::string bg_color;       // CodableRGBA JSON string, e.g. {"r":0.95,"g":0.87,"b":0.73,"a":1.0}
    std::string filter;         // Filter JSON string (serialized filter criteria)
    std::string frame;          // Frame JSON string (unused on Windows; local FrameStore manages position)
    std::string created_at;     // ISO 8601 datetime
    std::string updated_at;     // ISO 8601 datetime
};

} // namespace stickytodo::models
