#include "core/AppState.h"
#include "App.h"
#include "codec/JsonHelper.h"

#include <nlohmann/json.hpp>
#include <algorithm>

namespace stickytodo::core {

AppState::AppState() = default;
AppState::~AppState() { Shutdown(); }

void AppState::Initialize() {
    // Try to restore session from Credential Manager. If any stored
    // credential is present we set the HTTP client up and open the WS
    // connection. Token validity is NOT probed here via /health (that
    // route is unauthenticated — it succeeds even when the token is
    // expired, so a /health "ok" proves nothing about auth). Instead we
    // rely on two existing fail paths:
    //   • WS server rejects invalid/expired token with close code 4401
    //     → WsSignal::Unauthorized → HandleUnauthorized → Logout
    //   • Any authenticated REST call returning 401 invokes
    //     HttpClient::onUnauthorized_ → HandleUnauthorized → Logout
    // Both converge on the same teardown, so there is no duplicated path.
    auto creds = CredentialStore::LoadLast();
    if (creds.has_value() && !creds->token.empty()) {
        baseUrl_ = creds->base_url;
        username_ = creds->username;
        token_ = creds->token;

        http_.SetBaseUrl(baseUrl_);
        http_.SetToken(token_);
        http_.SetOnUnauthorized([this]() { HandleUnauthorized(); });

        ConnectWebSocket();
    }
}

void AppState::Shutdown() {
    DisconnectWebSocket();
}

bool AppState::Login(const std::string& baseUrl, const std::string& username, const std::string& password) {
    // Configure HTTP client for this attempt
    http_.SetBaseUrl(baseUrl);
    http_.SetToken("");
    http_.SetOnUnauthorized([this]() { HandleUnauthorized(); });

    auto result = http_.Login(username, password);
    if (!result.has_value()) {
        return false;
    }

    // Store credentials
    baseUrl_ = baseUrl;
    username_ = result->username;
    token_ = result->token;

    http_.SetToken(token_);

    // Persist to Credential Manager
    CredentialStore::Credentials creds;
    creds.username = username_;
    creds.token = token_;
    creds.base_url = baseUrl_;
    CredentialStore::Save(creds);

    // Notify auth change
    if (onAuthChanged_) onAuthChanged_(true);

    // Connect WebSocket
    ConnectWebSocket();

    return true;
}

void AppState::Logout() {
    DisconnectWebSocket();

    // Clear credentials from Credential Manager
    if (!username_.empty()) {
        CredentialStore::Delete(username_);
    }

    // Clear in-memory state
    token_.clear();
    username_.clear();
    http_.SetToken("");

    {
        std::lock_guard<std::mutex> lock(dataMutex_);
        stickies_.clear();
    }

    if (onAuthChanged_) onAuthChanged_(false);
    if (onStickiesChanged_) onStickiesChanged_();
}

std::string AppState::TestConnection(const std::string& baseUrl) {
    HttpClient testClient;
    testClient.SetBaseUrl(baseUrl);
    auto health = testClient.HealthCheck();
    if (health.has_value()) {
        return health->version;
    }
    return "";
}

std::vector<models::StickyNote> AppState::GetStickies() const {
    // Returned by value so callers on the UI thread get a stable snapshot
    // even though `stickies_` may still be concurrently mutated on the UI
    // thread itself via UI-thread-marshalled WS handlers. Cheap: the
    // vector is short (one entry per sticky on screen) and StickyNote is
    // a small POD of strings.
    std::lock_guard<std::mutex> lock(dataMutex_);
    return stickies_;
}

void AppState::FetchStickies() {
    if (!IsAuthenticated()) return;

    auto result = http_.ListStickies();
    if (result.has_value()) {
        std::lock_guard<std::mutex> lock(dataMutex_);
        stickies_ = std::move(result->items);
    }

    if (onStickiesChanged_) onStickiesChanged_();
}

void AppState::ConnectWebSocket() {
    ws_.SetBaseUrl(baseUrl_);
    ws_.SetToken(token_);
    // Worker-thread → UI-thread marshalling. The callbacks below run on
    // WebSocketClient's worker thread and MUST NOT touch HWNDs or
    // `stickies_`. They only PostMessageW the event/signal to the
    // UI-thread target (set earlier via SetUIThreadTarget). TrayIcon's
    // WndProc picks those messages up and calls back into
    // HandleWsEventOnUIThread / HandleWsSignalOnUIThread, which is where
    // the real mutation and fan-out happens.
    ws_.SetOnSignal([this](WsSignal sig) { PostWsSignalToUIThread(sig); });
    ws_.SetOnEvent([this](const WsEvent& ev) { PostWsEventToUIThread(ev); });
    ws_.Connect();
}

void AppState::DisconnectWebSocket() {
    ws_.Disconnect();
}

// --------- Worker-thread marshalling (NO state mutation here) -----------

void AppState::PostWsSignalToUIThread(WsSignal signal) {
    // If the UI thread hasn't registered a target yet (e.g. WS happened to
    // reconnect after App::Shutdown has run and nulled everything), drop
    // the signal on the floor — nothing is listening anyway.
    HWND target = uiThreadTarget_;
    if (!target) return;
    // WsSignal is a small enum — fits trivially in WPARAM. LPARAM unused.
    ::PostMessageW(target, WM_STICKYTODO_WS_SIGNAL,
                   static_cast<WPARAM>(signal), 0);
}

void AppState::PostWsEventToUIThread(const WsEvent& event) {
    HWND target = uiThreadTarget_;
    if (!target) return;
    // Deep-copy the event onto the heap and transfer ownership to the UI
    // thread via WPARAM. The receiving WndProc (TrayIcon::TrayWndProc)
    // MUST delete the pointer after handing it to
    // HandleWsEventOnUIThread.  Using new/delete rather than a smart
    // pointer because WPARAM is an integer and we need raw ownership
    // transfer semantics across the PostMessage boundary.
    auto* heap = new WsEvent(event);
    if (!::PostMessageW(target, WM_STICKYTODO_WS_EVENT,
                        reinterpret_cast<WPARAM>(heap), 0)) {
        // PostMessageW can fail if the target HWND was just destroyed
        // (race with shutdown). Avoid the leak.
        delete heap;
    }
}

// --------- UI-thread handlers (safe to touch HWNDs and `stickies_`) -----

void AppState::HandleWsSignalOnUIThread(WsSignal signal) {
    switch (signal) {
        case WsSignal::Ready:
            // First connection — fetch all data.
            FetchStickies();
            break;

        case WsSignal::Reconnected:
            // Reconnected after disconnect — refetch everything (Hub
            // doesn't buffer historical events; see AGENTS.md §3.3).
            FetchStickies();
            break;

        case WsSignal::Unauthorized:
            HandleUnauthorized();
            break;

        case WsSignal::Disconnected:
            // Connection lost; WebSocketClient will auto-reconnect via
            // exponential backoff. Nothing to do here.
            break;
    }
}

void AppState::HandleWsEventOnUIThread(const WsEvent& event) {
    if (event.type == "sticky.upserted") {
        MergeStickyUpserted(event.data);
    } else if (event.type == "sticky.deleted") {
        MergeStickyDeleted(event.id);
    } else if (event.type == "todo.created" ||
               event.type == "todo.updated" ||
               event.type == "todo.deleted") {
        // Todo events → precise fan-out to every sticky window. Each
        // window decides whether to refetch (its own filter will drop
        // irrelevant items). We stay well away from HWND_BROADCAST,
        // which would spam other applications' top-level windows.
        if (auto* app = GetApp()) {
            app->PostMessageToAllStickies(WM_STICKYTODO_REFRESH);
        }
    }
    // Unknown event types are intentionally ignored (forward-compatible
    // with future server-side additions; see AGENTS.md §3.3 for the
    // closed set of 5 event types currently defined).
}

void AppState::HandleUnauthorized() {
    // Token was rejected by server. Force logout, which clears token,
    // disconnects WS, wipes credential store, and notifies UI via
    // onAuthChanged_(false) / onStickiesChanged_(). The UI (App) will in
    // turn close all sticky windows via SyncStickyWindows().
    Logout();
}

void AppState::MergeStickyUpserted(const std::string& jsonData) {
    if (jsonData.empty()) {
        FetchStickies();
        return;
    }

    std::string noteId;
    try {
        auto j = nlohmann::json::parse(jsonData);
        auto note = codec::JsonHelper::ParseStickyNote(j);
        noteId = note.id;

        std::lock_guard<std::mutex> lock(dataMutex_);
        auto it = std::find_if(stickies_.begin(), stickies_.end(),
                               [&](const models::StickyNote& s) { return s.id == note.id; });
        if (it != stickies_.end()) {
            *it = note;
        } else {
            stickies_.push_back(note);
        }
    } catch (...) {
        FetchStickies();
        return;
    }

    if (onStickiesChanged_) onStickiesChanged_();

    // Tell the specific sticky window (if open) to refresh its data so the
    // title/bg/filter stay in sync. If the window is not open yet, the
    // onStickiesChanged_ callback owned by App will open it.
    if (!noteId.empty()) {
        if (auto* app = GetApp()) {
            app->PostMessageToSticky(noteId, WM_STICKYTODO_STICKY_UPSERTED);
        }
    }
}

void AppState::MergeStickyDeleted(const std::string& id) {
    {
        std::lock_guard<std::mutex> lock(dataMutex_);
        stickies_.erase(
            std::remove_if(stickies_.begin(), stickies_.end(),
                           [&](const models::StickyNote& s) { return s.id == id; }),
            stickies_.end());
    }
    if (onStickiesChanged_) onStickiesChanged_();

    // Route the delete to the matching sticky window so it can close itself.
    if (!id.empty()) {
        if (auto* app = GetApp()) {
            app->PostMessageToSticky(id, WM_STICKYTODO_STICKY_DELETED);
        }
    }
}

} // namespace stickytodo::core
