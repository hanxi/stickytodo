#include "codec/StickyCodec.h"

#include <nlohmann/json.hpp>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cmath>

namespace stickytodo::codec {

// ---------- RgbaColor ----------

uint32_t RgbaColor::ToColorRef() const {
    uint8_t r = static_cast<uint8_t>(std::clamp(red, 0.0, 1.0) * 255.0 + 0.5);
    uint8_t g = static_cast<uint8_t>(std::clamp(green, 0.0, 1.0) * 255.0 + 0.5);
    uint8_t b = static_cast<uint8_t>(std::clamp(blue, 0.0, 1.0) * 255.0 + 0.5);
    // COLORREF format: 0x00BBGGRR
    return static_cast<uint32_t>(r) | (static_cast<uint32_t>(g) << 8) | (static_cast<uint32_t>(b) << 16);
}

void RgbaColor::ToD2DColor(float& r, float& g, float& b, float& a) const {
    r = static_cast<float>(std::clamp(red, 0.0, 1.0));
    g = static_cast<float>(std::clamp(green, 0.0, 1.0));
    b = static_cast<float>(std::clamp(blue, 0.0, 1.0));
    a = static_cast<float>(std::clamp(alpha, 0.0, 1.0));
}

// ---------- StickyCodec: bg_color ----------

std::string StickyCodec::HexToBgColorJson(const std::string& hex) {
    std::string h = hex;
    // Strip leading '#'
    if (!h.empty() && h[0] == '#') {
        h = h.substr(1);
    }

    // Expand 3-char hex to 6-char
    if (h.size() == 3) {
        std::string expanded;
        expanded += h[0]; expanded += h[0];
        expanded += h[1]; expanded += h[1];
        expanded += h[2]; expanded += h[2];
        h = expanded;
    }

    if (h.size() < 6) {
        // Fallback: default sticky yellow
        return R"({"red":1.0,"green":0.92,"blue":0.54,"alpha":1.0})";
    }

    unsigned int rVal = 0, gVal = 0, bVal = 0;
    try {
        rVal = std::stoul(h.substr(0, 2), nullptr, 16);
        gVal = std::stoul(h.substr(2, 2), nullptr, 16);
        bVal = std::stoul(h.substr(4, 2), nullptr, 16);
    } catch (...) {
        return R"({"red":1.0,"green":0.92,"blue":0.54,"alpha":1.0})";
    }

    double red = rVal / 255.0;
    double green = gVal / 255.0;
    double blue = bVal / 255.0;

    nlohmann::json j;
    j["red"] = red;
    j["green"] = green;
    j["blue"] = blue;
    j["alpha"] = 1.0;
    return j.dump();
}

std::string StickyCodec::BgColorJsonToHex(const std::string& json) {
    RgbaColor color = ParseBgColor(json);

    uint8_t r = static_cast<uint8_t>(std::clamp(color.red, 0.0, 1.0) * 255.0 + 0.5);
    uint8_t g = static_cast<uint8_t>(std::clamp(color.green, 0.0, 1.0) * 255.0 + 0.5);
    uint8_t b = static_cast<uint8_t>(std::clamp(color.blue, 0.0, 1.0) * 255.0 + 0.5);

    std::ostringstream oss;
    oss << "#" << std::uppercase << std::hex << std::setfill('0')
        << std::setw(2) << static_cast<int>(r)
        << std::setw(2) << static_cast<int>(g)
        << std::setw(2) << static_cast<int>(b);
    return oss.str();
}

RgbaColor StickyCodec::ParseBgColor(const std::string& json) {
    RgbaColor color;
    if (json.empty()) {
        // Default: sticky yellow
        color.red = 1.0;
        color.green = 0.92;
        color.blue = 0.54;
        color.alpha = 1.0;
        return color;
    }

    try {
        auto j = nlohmann::json::parse(json);
        color.red = j.value("red", 1.0);
        color.green = j.value("green", 0.92);
        color.blue = j.value("blue", 0.54);
        color.alpha = j.value("alpha", 1.0);
    } catch (...) {
        // Parsing failed, return default
        color.red = 1.0;
        color.green = 0.92;
        color.blue = 0.54;
        color.alpha = 1.0;
    }
    return color;
}

std::string StickyCodec::RgbaToJson(const RgbaColor& color) {
    nlohmann::json j;
    j["red"] = color.red;
    j["green"] = color.green;
    j["blue"] = color.blue;
    j["alpha"] = color.alpha;
    return j.dump();
}

// ---------- StickyCodec: filter ----------

std::string StickyCodec::FilterToJson(const models::Filter& filter) {
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

models::Filter StickyCodec::JsonToFilter(const std::string& json) {
    models::Filter filter;
    if (json.empty() || json == "{}") {
        return filter;
    }

    try {
        auto j = nlohmann::json::parse(json);
        if (j.contains("status") && j["status"].is_string()) {
            filter.status = j["status"].get<std::string>();
        }
        if (j.contains("tag") && j["tag"].is_string()) {
            filter.tag = j["tag"].get<std::string>();
        }
        if (j.contains("keyword") && j["keyword"].is_string()) {
            filter.keyword = j["keyword"].get<std::string>();
        }
        if (j.contains("due_before") && j["due_before"].is_string()) {
            filter.due_before = j["due_before"].get<std::string>();
        }
        if (j.contains("include_deleted") && j["include_deleted"].is_boolean()) {
            filter.include_deleted = j["include_deleted"].get<bool>();
        }
        if (j.contains("only_deleted") && j["only_deleted"].is_boolean()) {
            filter.only_deleted = j["only_deleted"].get<bool>();
        }
        if (j.contains("page") && j["page"].is_number_integer()) {
            filter.page = j["page"].get<int>();
        }
        if (j.contains("page_size") && j["page_size"].is_number_integer()) {
            filter.page_size = j["page_size"].get<int>();
        }
    } catch (...) {
        // Return default filter on parse error
    }
    return filter;
}

} // namespace stickytodo::codec
