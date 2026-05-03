#pragma once

#include <string>
#include <unordered_map>
#include <optional>

namespace stickytodo::core {

/// Window position/size rectangle, aligned with macOS CodableRect.
struct FrameRect {
    double x = 100.0;
    double y = 100.0;
    double width = 300.0;
    double height = 420.0;
};

/// Persists sticky window positions to %APPDATA%\stickytodo\frames.json.
/// Analogous to macOS FrameStore (client/mac/stickytodo/Storage/FrameStore.swift).
///
/// Storage format: { "<sticky_id>": {"x":..,"y":..,"width":..,"height":..}, ... }
class FrameStore {
public:
    FrameStore();

    /// Load all stored frames from disk.
    std::unordered_map<std::string, FrameRect> LoadAll();

    /// Load frame for a specific sticky note. Returns nullopt if not found.
    std::optional<FrameRect> Load(const std::string& stickyId);

    /// Save frame for a specific sticky note. Persists immediately to disk.
    void Save(const std::string& stickyId, const FrameRect& rect);

    /// Remove stored frame for a sticky note.
    void Remove(const std::string& stickyId);

    /// Remove frames for sticky IDs that no longer exist.
    /// Returns number of orphans pruned.
    int PruneOrphans(const std::vector<std::string>& aliveIds);

private:
    std::string GetFilePath() const;
    void PersistAll(const std::unordered_map<std::string, FrameRect>& frames);

    std::string filePath_;
};

} // namespace stickytodo::core
