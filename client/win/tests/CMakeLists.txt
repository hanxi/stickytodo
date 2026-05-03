# Unit tests for the Windows client.
#
# Scope: codec round-trip (StickyCodec.Hex ↔ CodableRGBA JSON,
#        StickyCodec.Filter ↔ JSON) and JsonHelper DTO round-trip (Todo /
#        StickyNote / AuditLog). These are pure-logic tests — no D2D, no
#        HTTP, no filesystem. Anything requiring Direct2D or Win32 message
#        pumping belongs in a smoke-test binary, not here.
#
# Built only when the parent CMakeLists sets BUILD_TESTS=ON (off by default
# so the main IDE workflow stays cheap). CI runs it explicitly via
# `cmake --preset ... -DBUILD_TESTS=ON` + `ctest`.

find_package(GTest CONFIG REQUIRED)

# ----- test_sticky_codec ---------------------------------------------------
#
# Covers:
#   * HexToBgColorJson / BgColorJsonToHex round-trip (including #XYZ short
#     form, leading-# stripping, malformed input fallback to default sticky
#     yellow)
#   * FilterToJson / JsonToFilter round-trip (including omit-default keys on
#     encode, tolerate-missing-keys + tolerate-malformed-JSON on decode)
#   * ParseBgColor default-on-empty-string contract (macOS stickyCodec.ts
#     and Swift `defaultStickyColor` guarantee the same)

add_executable(test_sticky_codec
    test_sticky_codec.cpp
    # Pull in the units under test directly — cheaper than linking the full
    # stickytodo executable, and keeps the test binary free of Direct2D /
    # Win32 dependencies it would never exercise.
    ../src/codec/StickyCodec.cpp
)
target_include_directories(test_sticky_codec PRIVATE ../src)
target_link_libraries(test_sticky_codec PRIVATE
    GTest::gtest
    GTest::gtest_main
    nlohmann_json::nlohmann_json
)
add_test(NAME sticky_codec COMMAND test_sticky_codec)

# ----- test_models_json ----------------------------------------------------
#
# Covers:
#   * JsonHelper::ParseTodo / TodoToJson round-trip
#   * JsonHelper::ParseStickyNote / StickyNoteToJson round-trip (including
#     the "frame is client-local so always serialize as {}" contract — see
#     AGENTS.md §3.4 cloud-source refactor)
#   * JsonHelper::ParseAuditLog (no encode — the client never sends audit
#     logs back to the server)
#   * ParseTodos / ParseStickyNotes accepting both bare array and the
#     paginated `{"items": [...], "total": N}` envelope

add_executable(test_models_json
    test_models_json.cpp
    ../src/codec/JsonHelper.cpp
)
target_include_directories(test_models_json PRIVATE ../src)
target_link_libraries(test_models_json PRIVATE
    GTest::gtest
    GTest::gtest_main
    nlohmann_json::nlohmann_json
)
add_test(NAME models_json COMMAND test_models_json)
