// test_sticky_codec.cpp — StickyCodec round-trip and robustness tests.
//
// Scope: the bg_color (hex ↔ CodableRGBA JSON) and filter (Filter struct ↔
// JSON) conversions in codec/StickyCodec.cpp. Nothing else in the Windows
// client touches these conversions, so exhaustive coverage here is the
// first line of defence against the "pure JSON schema drift between Web /
// macOS / Windows" class of bug.
//
// The fixture values mirror what macOS's DEFAULT_STICKY_COLOR and Web's
// stickyCodec.ts emit, so cross-platform drift would show up as test
// failures here before it ever lands in a sticky on disk.

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "codec/StickyCodec.h"
#include "models/Filter.h"

#include <cmath>
#include <string>

using stickytodo::codec::StickyCodec;
using stickytodo::codec::RgbaColor;
using stickytodo::models::Filter;

namespace {

/// Parse a bg_color JSON and compare each channel with the given expected
/// values to 1/255 precision — same rounding bucket the Hex ↔ RGBA
/// conversion uses internally.
void ExpectRgbaNear(const std::string& json, double r, double g, double b, double a) {
    RgbaColor c = StickyCodec::ParseBgColor(json);
    EXPECT_NEAR(c.red,   r, 1.0 / 255.0) << "red mismatch in " << json;
    EXPECT_NEAR(c.green, g, 1.0 / 255.0) << "green mismatch in " << json;
    EXPECT_NEAR(c.blue,  b, 1.0 / 255.0) << "blue mismatch in " << json;
    EXPECT_NEAR(c.alpha, a, 1.0 / 255.0) << "alpha mismatch in " << json;
}

} // namespace

// =============== BgColor: Hex → JSON ===========================================

TEST(StickyCodec_HexToBgColorJson, FullSixCharHexProducesValidRgba) {
    // Sticky Yellow preset — the single most important colour in the whole app.
    std::string json = StickyCodec::HexToBgColorJson("#FFEB8A");
    ExpectRgbaNear(json, 1.0, 0.92, 0.54, 1.0);
}

TEST(StickyCodec_HexToBgColorJson, LeadingHashIsOptional) {
    std::string withHash = StickyCodec::HexToBgColorJson("#FFEB8A");
    std::string noHash   = StickyCodec::HexToBgColorJson("FFEB8A");
    // Both strings must parse to equivalent RGBA values.
    auto a = nlohmann::json::parse(withHash);
    auto b = nlohmann::json::parse(noHash);
    EXPECT_EQ(a, b);
}

TEST(StickyCodec_HexToBgColorJson, ThreeCharShortFormExpands) {
    // #F0C is shorthand for #FF00CC — classic CSS short-hex expansion.
    std::string json = StickyCodec::HexToBgColorJson("#F0C");
    ExpectRgbaNear(json, 1.0, 0.0, 0.8, 1.0);
}

TEST(StickyCodec_HexToBgColorJson, LowerCaseDigitsAccepted) {
    std::string upper = StickyCodec::HexToBgColorJson("#ABCDEF");
    std::string lower = StickyCodec::HexToBgColorJson("#abcdef");
    auto a = nlohmann::json::parse(upper);
    auto b = nlohmann::json::parse(lower);
    EXPECT_EQ(a, b);
}

TEST(StickyCodec_HexToBgColorJson, MalformedInputFallsBackToStickyYellow) {
    // Any unparseable hex must fall back to the default sticky yellow so
    // rendering always has something sane to draw with — matches macOS's
    // `DEFAULT_STICKY_COLOR` fallback in stickyCodec.ts / StickyNote.swift.
    std::string json = StickyCodec::HexToBgColorJson("#ZZYYXX");
    ExpectRgbaNear(json, 1.0, 0.92, 0.54, 1.0);
}

TEST(StickyCodec_HexToBgColorJson, TooShortInputFallsBack) {
    std::string json = StickyCodec::HexToBgColorJson("#AB");
    ExpectRgbaNear(json, 1.0, 0.92, 0.54, 1.0);
}

TEST(StickyCodec_HexToBgColorJson, EmptyStringFallsBack) {
    std::string json = StickyCodec::HexToBgColorJson("");
    ExpectRgbaNear(json, 1.0, 0.92, 0.54, 1.0);
}

// =============== BgColor: JSON → Hex ===========================================

TEST(StickyCodec_BgColorJsonToHex, RoundTripsKnownColors) {
    // Exhaustively round-trip every preset colour. If any of these fails
    // the UI picker and the on-disk data would disagree about the colour.
    const char* presets[] = { "#FFEB8A", "#C7F2D4", "#CCE6FA", "#FCD4E1", "#E1D1FA" };
    for (const char* hex : presets) {
        std::string json = StickyCodec::HexToBgColorJson(hex);
        std::string back = StickyCodec::BgColorJsonToHex(json);
        EXPECT_EQ(back, hex) << "round-trip mismatch for " << hex;
    }
}

TEST(StickyCodec_BgColorJsonToHex, HexOutputIsUppercase) {
    std::string json = StickyCodec::HexToBgColorJson("#abcdef");
    EXPECT_EQ(StickyCodec::BgColorJsonToHex(json), "#ABCDEF");
}

TEST(StickyCodec_BgColorJsonToHex, EmptyJsonReturnsDefaultYellowHex) {
    // Sticky Yellow hex is #FFEB8A (r=255, g=0.92*255≈235, b=0.54*255≈138).
    EXPECT_EQ(StickyCodec::BgColorJsonToHex(""), "#FFEB8A");
}

TEST(StickyCodec_BgColorJsonToHex, MalformedJsonReturnsDefaultYellowHex) {
    EXPECT_EQ(StickyCodec::BgColorJsonToHex("{not json"), "#FFEB8A");
}

// =============== BgColor: ParseBgColor ========================================

TEST(StickyCodec_ParseBgColor, EmptyStringYieldsDefaultSticky) {
    RgbaColor c = StickyCodec::ParseBgColor("");
    EXPECT_DOUBLE_EQ(c.red,   1.0);
    EXPECT_DOUBLE_EQ(c.green, 0.92);
    EXPECT_DOUBLE_EQ(c.blue,  0.54);
    EXPECT_DOUBLE_EQ(c.alpha, 1.0);
}

TEST(StickyCodec_ParseBgColor, MissingChannelFallsBackToChannelDefault) {
    // Partial JSON: only provide red. Other channels must fall back to the
    // sticky-yellow defaults, not zero. This protects against dirty cloud
    // data that's been manually edited on one platform.
    std::string partial = R"({"red":0.5})";
    RgbaColor c = StickyCodec::ParseBgColor(partial);
    EXPECT_DOUBLE_EQ(c.red,   0.5);
    EXPECT_DOUBLE_EQ(c.green, 0.92);
    EXPECT_DOUBLE_EQ(c.blue,  0.54);
    EXPECT_DOUBLE_EQ(c.alpha, 1.0);
}

TEST(StickyCodec_RgbaToJson_ThenParseBgColor, RoundTrip) {
    RgbaColor src;
    src.red = 0.25; src.green = 0.50; src.blue = 0.75; src.alpha = 1.0;
    std::string json = StickyCodec::RgbaToJson(src);
    RgbaColor dst = StickyCodec::ParseBgColor(json);
    EXPECT_DOUBLE_EQ(dst.red,   src.red);
    EXPECT_DOUBLE_EQ(dst.green, src.green);
    EXPECT_DOUBLE_EQ(dst.blue,  src.blue);
    EXPECT_DOUBLE_EQ(dst.alpha, src.alpha);
}

// =============== RgbaColor helpers ============================================

TEST(RgbaColor_ToColorRef, EncodesAsBbggrr) {
    RgbaColor c; c.red = 1.0; c.green = 0.0; c.blue = 0.0; c.alpha = 1.0;
    // COLORREF is 0x00BBGGRR — pure red must be 0x000000FF.
    EXPECT_EQ(c.ToColorRef() & 0x00FFFFFFu, 0x000000FFu);
}

TEST(RgbaColor_ToD2DColor, ClampsOutOfRangeChannels) {
    // Dirty input that somehow sneaks in >1.0 or <0.0 channels must still
    // produce a drawable D2D colour instead of propagating garbage to the
    // renderer.
    RgbaColor c; c.red = 1.5; c.green = -0.3; c.blue = 0.5; c.alpha = 2.0;
    float r, g, b, a;
    c.ToD2DColor(r, g, b, a);
    EXPECT_FLOAT_EQ(r, 1.0f);
    EXPECT_FLOAT_EQ(g, 0.0f);
    EXPECT_FLOAT_EQ(b, 0.5f);
    EXPECT_FLOAT_EQ(a, 1.0f);
}

// =============== Filter: struct → JSON ========================================

TEST(StickyCodec_FilterToJson, DefaultFilterOmitsAllDefaultKeys) {
    // A freshly-constructed Filter must serialise to {} — defaults aren't
    // persisted, which keeps the stored JSON compact AND means a future
    // default change doesn't silently rewrite existing stickies' data.
    Filter f;
    std::string json = StickyCodec::FilterToJson(f);
    auto j = nlohmann::json::parse(json);
    EXPECT_TRUE(j.empty()) << "default Filter must produce {} but got " << json;
}

TEST(StickyCodec_FilterToJson, OnlyNonDefaultKeysAppear) {
    Filter f;
    f.status = "pending";
    f.tag = "work";
    f.keyword = "foo";
    f.include_deleted = true;
    f.page_size = 80;
    // page=1 (default) and only_deleted=false (default) must NOT appear.
    std::string json = StickyCodec::FilterToJson(f);
    auto j = nlohmann::json::parse(json);
    EXPECT_EQ(j.size(), 5u);
    EXPECT_EQ(j.value("status", ""), "pending");
    EXPECT_EQ(j.value("tag", ""), "work");
    EXPECT_EQ(j.value("keyword", ""), "foo");
    EXPECT_EQ(j.value("include_deleted", false), true);
    EXPECT_EQ(j.value("page_size", 0), 80);
    EXPECT_FALSE(j.contains("page"));
    EXPECT_FALSE(j.contains("only_deleted"));
}

// =============== Filter: JSON → struct ========================================

TEST(StickyCodec_JsonToFilter, EmptyOrBraceReturnsDefault) {
    Filter a = StickyCodec::JsonToFilter("");
    Filter b = StickyCodec::JsonToFilter("{}");
    Filter defaults;
    EXPECT_EQ(a.status, defaults.status);
    EXPECT_EQ(a.page_size, defaults.page_size);
    EXPECT_EQ(b.status, defaults.status);
    EXPECT_EQ(b.page_size, defaults.page_size);
}

TEST(StickyCodec_JsonToFilter, MalformedJsonReturnsDefault) {
    // Dirty cloud data must never blow up the UI — the contract is "always
    // yield a valid Filter, even if that means ignoring the input entirely".
    Filter f = StickyCodec::JsonToFilter("{not valid json");
    Filter defaults;
    EXPECT_EQ(f.status, defaults.status);
    EXPECT_EQ(f.page_size, defaults.page_size);
}

TEST(StickyCodec_JsonToFilter, ParsesAllKnownKeys) {
    std::string src = R"({
        "status":"done",
        "tag":"home",
        "keyword":"grocery",
        "due_before":"2026-01-01T00:00:00Z",
        "include_deleted":true,
        "only_deleted":true,
        "page":3,
        "page_size":100
    })";
    Filter f = StickyCodec::JsonToFilter(src);
    EXPECT_EQ(f.status, "done");
    EXPECT_EQ(f.tag, "home");
    EXPECT_EQ(f.keyword, "grocery");
    EXPECT_EQ(f.due_before, "2026-01-01T00:00:00Z");
    EXPECT_TRUE(f.include_deleted);
    EXPECT_TRUE(f.only_deleted);
    EXPECT_EQ(f.page, 3);
    EXPECT_EQ(f.page_size, 100);
}

TEST(StickyCodec_FilterRoundTrip, LosslessForNonDefaultValues) {
    Filter src;
    src.status = "pending";
    src.tag = "quick";
    src.keyword = "ship";
    src.due_before = "2025-12-31T23:59:59Z";
    src.include_deleted = true;
    src.only_deleted = false;
    src.page = 1;        // default — will be dropped by encoder
    src.page_size = 25;

    Filter dst = StickyCodec::JsonToFilter(StickyCodec::FilterToJson(src));
    EXPECT_EQ(dst.status, src.status);
    EXPECT_EQ(dst.tag, src.tag);
    EXPECT_EQ(dst.keyword, src.keyword);
    EXPECT_EQ(dst.due_before, src.due_before);
    EXPECT_EQ(dst.include_deleted, src.include_deleted);
    EXPECT_EQ(dst.only_deleted, src.only_deleted);
    EXPECT_EQ(dst.page, src.page);           // default preserved
    EXPECT_EQ(dst.page_size, src.page_size);
}

TEST(StickyCodec_Filter_IsEmpty, CorrectlyReportsEmptyVsNonEmpty) {
    Filter defaults;
    EXPECT_TRUE(defaults.IsEmpty());
    Filter withStatus; withStatus.status = "pending";
    EXPECT_FALSE(withStatus.IsEmpty());
    Filter onlyPageSize; onlyPageSize.page_size = 100;
    // page_size is pagination, not a filter criterion — IsEmpty reports
    // only substantive filters.
    EXPECT_TRUE(onlyPageSize.IsEmpty());
}
