#pragma once

#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>

namespace stickytodo::core {

/// WebSocket connection signals.
enum class WsSignal {
    Ready,          // First connection established, auth succeeded
    Reconnected,    // Reconnected after disconnect (caller should refetch all data)
    Disconnected,   // Connection lost
    Unauthorized    // Server sent close code 4401 (token invalid/expired)
};

/// WebSocket event received from server.
struct WsEvent {
    std::string type;   // e.g. "todo.created", "sticky.upserted", "ready"
    std::string data;   // raw JSON string of the "data" field (may be empty)
    std::string id;     // resource ID (string for sticky, numeric string for todo)
};

/// WebSocket client for real-time event push.
/// Protocol:
///   1. Connect to ws[s]://<host>/api/ws
///   2. Send auth frame: {"type":"auth","token":"<jwt>"}
///   3. Wait for ready frame: {"type":"ready"}
///   4. Receive event frames: {"type":"<event_type>","data":{...},"id":"..."}
///
/// Reconnection: exponential backoff [1,2,4,8,16,30] seconds.
/// Close code 4401 = Unauthorized → do NOT reconnect, signal Unauthorized.
///
/// Keep-alive policy (divergence from macOS RealtimeClient — intentional):
///   The Go server actively sends a WebSocket ping every 30 s (see server
///   `ws/client.go#pingPeriod`) and expects a pong within `pongWait=60s`.
///   WinHTTP's WebSocket stack responds to incoming ping frames automatically
///   (there is no handler we need to register) and, critically, WinHTTP does
///   NOT expose any API that emits the WebSocket ping opcode (0x9) — only
///   data frames (text/binary/close/fragments). Any home-grown "empty frame"
///   sent from the client would be delivered to the server as an unsolicited
///   upstream business message, which `/api/ws` strictly forbids (server
///   closes with code 4400 on any non-auth upstream frame, per AGENTS.md
///   §3.3). Therefore we rely on the server-ping → WinHTTP-auto-pong path
///   alone. macOS's extra client-side `sendPing()` has no Windows analogue
///   and MUST NOT be emulated with a text-frame stand-in.
class WebSocketClient {
public:
    using SignalCallback = std::function<void(WsSignal)>;
    using EventCallback = std::function<void(const WsEvent&)>;

    WebSocketClient();
    ~WebSocketClient();

    /// Set the server base URL (http/https → ws/wss conversion is automatic).
    void SetBaseUrl(const std::string& url);

    /// Set the JWT token for authentication.
    void SetToken(const std::string& token);

    /// Set the HTTP proxy URL (e.g. "http://127.0.0.1:7890"). Empty
    /// string disables the proxy and falls back to system / IE default.
    /// Picked up at the start of the next ConnectOnce() — already-live
    /// connections are NOT re-routed; callers that want immediate effect
    /// should follow up with Disconnect()+Connect() or rely on the
    /// auto-reconnect path. Mirrors HttpClient::SetProxy.
    void SetProxy(const std::string& proxy);

    /// Set callback for connection state signals.
    void SetOnSignal(SignalCallback cb);

    /// Set callback for business events.
    void SetOnEvent(EventCallback cb);

    /// Start the WebSocket connection. Spawns a background thread.
    void Connect();

    /// Disconnect and stop all background activity.
    void Disconnect();

    /// Returns true if currently connected and authenticated.
    bool IsConnected() const { return connected_.load(); }

private:
    void WorkerLoop();
    bool ConnectOnce();
    void ReceiveLoop(void* hWebSocket);
    // Returns a Win32 error code; 0 (ERROR_SUCCESS) means the auth frame
    // was queued to the WinHTTP stack. A non-zero return means the
    // handshake failed BEFORE the receive loop started — caller must
    // treat this connection attempt as failed and fall through to the
    // exponential-backoff reconnect in WorkerLoop.
    unsigned long SendAuthFrame(void* hWebSocket);
    std::string BuildWsUrl() const;

    // Guards baseUrl_ / token_ / onSignal_ / onEvent_ against concurrent
    // access between the UI thread (which calls Set* during login / logout
    // / callback registration) and the worker thread (which reads them from
    // ConnectOnce / SendAuthFrame / ReceiveLoop). Must be declared BEFORE
    // the members it protects so that those members are destroyed first
    // during ~WebSocketClient (C++ destroys in reverse declaration order),
    // and must be declared BEFORE workerThread_ so Disconnect()'s join
    // happens before the mutex goes away.
    mutable std::mutex mutex_;

    std::string baseUrl_;
    std::string token_;
    SignalCallback onSignal_;
    EventCallback onEvent_;

    std::thread workerThread_;
    std::atomic<bool> shouldRun_{false};
    std::atomic<bool> connected_{false};

    // Published by ConnectOnce right after WinHttpWebSocketCompleteUpgrade
    // succeeds. Read by Disconnect() on a different thread so it can
    // force-unblock a WinHttpWebSocketReceive that would otherwise only
    // return when the server closes the stream. Ownership transfer uses
    // exchange(nullptr): whichever thread successfully atomically takes
    // the non-null value is the owner that must call WinHttpCloseHandle.
    // Typed as void* (rather than HINTERNET) to avoid pulling winhttp.h
    // into the public header.
    std::atomic<void*> liveWebSocket_{nullptr};

    // Reconnection bookkeeping — touched only from the worker thread.
    int reconnectAttempt_ = 0;
    bool hadSuccessfulConnection_ = false;

    static constexpr int kBackoffSeconds[] = {1, 2, 4, 8, 16, 30};
    static constexpr int kBackoffCount = 6;
    static constexpr int kCloseCodeUnauthorized = 4401;
};

} // namespace stickytodo::core
