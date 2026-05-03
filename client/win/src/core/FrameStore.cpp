#include "core/FrameStore.h"

#include <nlohmann/json.hpp>
#include <windows.h>
// Required explicitly for CoTaskMemFree used below to release the wide-path
// buffer handed out by SHGetKnownFolderPath. Project-wide WIN32_LEAN_AND_MEAN
// means <windows.h> no longer transitively pulls in COM basics, and while
// <shlobj.h> does declare SHGetKnownFolderPath it is not guaranteed to
// forward-declare CoTaskMemFree on every Windows SDK revision. Being
// explicit here avoids the same C3861 class of error that caught main.cpp
// on the CI build.
#include <objbase.h>
#include <shlobj.h>
#include <fstream>
#include <filesystem>
#include <unordered_set>
#include <vector>

namespace stickytodo::core {

FrameStore::FrameStore() {
    filePath_ = GetFilePath();
    // Ensure directory exists
    std::filesystem::path dir = std::filesystem::path(filePath_).parent_path();
    std::filesystem::create_directories(dir);
}

std::string FrameStore::GetFilePath() const {
    wchar_t* appDataPath = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appDataPath))) {
        int sz = WideCharToMultiByte(CP_UTF8, 0, appDataPath, -1, nullptr, 0, nullptr, nullptr);
        std::string path(sz - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, appDataPath, -1, path.data(), sz, nullptr, nullptr);
        CoTaskMemFree(appDataPath);
        return path + "\\stickytodo\\frames.json";
    }
    // Fallback
    return "frames.json";
}

std::unordered_map<std::string, FrameRect> FrameStore::LoadAll() {
    std::unordered_map<std::string, FrameRect> result;

    std::ifstream file(filePath_);
    if (!file.is_open()) {
        return result;
    }

    try {
        nlohmann::json j;
        file >> j;
        if (!j.is_object()) return result;

        for (auto& [key, val] : j.items()) {
            if (!val.is_object()) continue;
            FrameRect rect;
            rect.x = val.value("x", 100.0);
            rect.y = val.value("y", 100.0);
            rect.width = val.value("width", 300.0);
            rect.height = val.value("height", 420.0);
            result[key] = rect;
        }
    } catch (...) {
        // Corrupted file — return empty
    }
    return result;
}

std::optional<FrameRect> FrameStore::Load(const std::string& stickyId) {
    auto all = LoadAll();
    auto it = all.find(stickyId);
    if (it != all.end()) {
        return it->second;
    }
    return std::nullopt;
}

void FrameStore::Save(const std::string& stickyId, const FrameRect& rect) {
    auto all = LoadAll();
    all[stickyId] = rect;
    PersistAll(all);
}

void FrameStore::Remove(const std::string& stickyId) {
    auto all = LoadAll();
    all.erase(stickyId);
    PersistAll(all);
}

int FrameStore::PruneOrphans(const std::vector<std::string>& aliveIds) {
    auto all = LoadAll();
    std::unordered_set<std::string> alive(aliveIds.begin(), aliveIds.end());
    int pruned = 0;
    for (auto it = all.begin(); it != all.end();) {
        if (alive.find(it->first) == alive.end()) {
            it = all.erase(it);
            ++pruned;
        } else {
            ++it;
        }
    }
    if (pruned > 0) {
        PersistAll(all);
    }
    return pruned;
}

void FrameStore::PersistAll(const std::unordered_map<std::string, FrameRect>& frames) {
    nlohmann::json j = nlohmann::json::object();
    for (const auto& [id, rect] : frames) {
        j[id] = {
            {"x", rect.x},
            {"y", rect.y},
            {"width", rect.width},
            {"height", rect.height}
        };
    }

    std::ofstream file(filePath_);
    if (file.is_open()) {
        file << j.dump(2);
    }
}

} // namespace stickytodo::core
