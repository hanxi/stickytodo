#pragma once

#include "models/StickyNote.h"
#include "models/Filter.h"

#include <string>
#include <cstdint>

namespace stickytodo::codec {

/// Color represented as RGBA components in [0,1] range.
/// Matches the backend/macOS "CodableRGBA" format:
///   {"red":1.0,"green":0.92,"blue":0.54,"alpha":1.0}
struct RgbaColor {
    double red = 1.0;
    double green = 1.0;
    double blue = 1.0;
    double alpha = 1.0;

    /// Convert to Win32 COLORREF (0x00BBGGRR). Alpha is discarded.
    uint32_t ToColorRef() const;

    /// Convert to Direct2D-compatible float components [0,1].
    void ToD2DColor(float& r, float& g, float& b, float& a) const;
};

/// Codec for StickyNote bg_color and filter JSON fields.
/// Aligns with:
///   - macOS: CodableRGBA struct in StickyNote.swift
///   - Web: client/web/src/lib/stickyCodec.ts
class StickyCodec {
public:
    // ---------- bg_color: hex ↔ CodableRGBA JSON ----------

    /// Convert hex color string (e.g. "#FFEB8A") to CodableRGBA JSON string.
    /// Output: {"red":1.0,"green":0.92,"blue":0.54,"alpha":1.0}
    static std::string HexToBgColorJson(const std::string& hex);

    /// Convert CodableRGBA JSON string to hex color string (e.g. "#FFEB8A").
    /// Input: {"red":1.0,"green":0.92,"blue":0.54,"alpha":1.0}
    static std::string BgColorJsonToHex(const std::string& json);

    /// Parse CodableRGBA JSON string to RgbaColor struct.
    static RgbaColor ParseBgColor(const std::string& json);

    /// Convert RgbaColor struct to CodableRGBA JSON string.
    static std::string RgbaToJson(const RgbaColor& color);

    // ---------- filter: Filter struct ↔ JSON ----------

    /// Serialize Filter struct to JSON string (snake_case keys).
    static std::string FilterToJson(const models::Filter& filter);

    /// Deserialize JSON string to Filter struct.
    static models::Filter JsonToFilter(const std::string& json);
};

} // namespace stickytodo::codec
