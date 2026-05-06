#include "core/WebSocketClient.h"

#include <nlohmann/json.hpp>
#include <windows.h>
#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")

namespace stickytodo::core {

static std::wstring Utf8ToWide(const std::string& str) {
    if (str.empty()) return L"";
    int sz = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), static_cast<int>(str.size()), nullptr, 0);
    std::wstring result(sz, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), static_cast<int>(str.size()), result.data(), sz);
    return result;
}

static std::string WideToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return "";
    int sz = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), static_cast<int>(wstr.size()), nullptr, 0, nullptr, nullptr);
    std::string result(sz, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), static_cast<int>(wstr.size()), result.data(), sz, nullptr, nullptr);
    return result;
}

WebSocketClient::WebSocketClient() = default;

WebSocketClient::~WebSocketClient() {
    Disconnect();
}

void WebSocketClient::SetBaseUrl(const std::string& url) {
    std::lock_guard<std::mutex> lock(mutex_);
    baseUrl_ = url;
}

void WebSocketClient::SetToken(const std::string& token) {
    std::lock_guard<std::mutex> lock(mutex_);
    token_ = token;
}

void WebSocketClient::SetProxy(const std::string& proxy) {
    // Lock to publish to the worker thread (which reads proxy_ inside
    // ConnectOnce under the same mutex). Note: this only takes effect on
    // the NEXT ConnectOnce — currently-live sessions stay on the old
    // proxy until they reconnect. AppState::SetHttpProxy is responsible
    // for forcing a Disconnect() so the auto-reconnect path picks up the
    // new value within seconds.
    std::lock_guard<std::mutex> lock(mutex_);
    proxy_ = proxy;
}

void WebSocketClient::SetOnSignal(SignalCallback cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    onSignal_ = std::move(cb);
}

void WebSocketClient::SetOnEvent(EventCallback cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    onEvent_ = std::move(cb);
}

std::string WebSocketClient::BuildWsUrl() const {
    std::string url = baseUrl_;
    // http → ws, https → wss
    if (url.find("https://") == 0) {
        url = "wss://" + url.substr(8);
    } else if (url.find("http://") == 0) {
        url = "ws://" + url.substr(7);
    }
    // Remove trailing slash
    while (!url.empty() && url.back() == '/') url.pop_back();
    url += "/api/ws";
    return url;
}

void WebSocketClient::Connect() {
    if (shouldRun_.load()) return; // Already running

    shouldRun_ = true;
    hadSuccessfulConnection_ = false;
    reconnectAttempt_ = 0;

    workerThread_ = std::thread([this]() { WorkerLoop(); });
}

void WebSocketClient::Disconnect() {
    shouldRun_ = false;
    connected_ = false;

    // Force-unblock any in-flight WinHttpWebSocketReceive on the worker
    // thread. Without this, Disconnect() would hang indefinitely waiting
    // for the server to close the stream (shouldRun_ alone only stops
    // the outer loop — it cannot abort a blocking WinHTTP call).
    //
    // Ownership rule: whichever thread atomically exchanges a non-null
    // value out of liveWebSocket_ is responsible for the handle's
    // WinHttpCloseHandle. ConnectOnce publishes the handle right after
    // upgrade; on clean teardown ConnectOnce takes it back with
    // exchange(nullptr) after ReceiveLoop returns. Here we race it:
    // if we win (Disconnect got there first), we close and the worker
    // thread's exchange(nullptr) yields nullptr, so it skips the close.
    // If ConnectOnce wins (receive loop already returned normally), we
    // get nullptr here and do nothing — handle is already being closed
    // on the worker. Either way, exactly one close happens.
    void* handle = liveWebSocket_.exchange(nullptr);
    if (handle) {
        // WinHttpCloseHandle on a live WebSocket handle cancels any
        // pending I/O (WinHttpWebSocketReceive returns with
        // ERROR_WINHTTP_OPERATION_CANCELLED) then destroys the handle.
        WinHttpCloseHandle(static_cast<HINTERNET>(handle));
    }

    if (workerThread_.joinable()) {
        workerThread_.join();
    }
}

void WebSocketClient::WorkerLoop() {
    while (shouldRun_.load()) {
        bool success = ConnectOnce();

        if (!shouldRun_.load()) break;

        if (success) {
            // Connection was established then lost
            connected_ = false;
            if (onSignal_) onSignal_(WsSignal::Disconnected);
        }

        if (!shouldRun_.load()) break;

        // Exponential backoff
        int backoffIdx = (reconnectAttempt_ < kBackoffCount) ? reconnectAttempt_ : kBackoffCount - 1;
        int delaySec = kBackoffSeconds[backoffIdx];
        reconnectAttempt_++;

        // Sleep in small increments so we can exit quickly on Disconnect()
        for (int i = 0; i < delaySec * 10 && shouldRun_.load(); ++i) {
            Sleep(100);
        }
    }
}

bool WebSocketClient::ConnectOnce() {
    std::string wsUrl;
    std::string token;
    std::string proxy;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        wsUrl = BuildWsUrl();
        token = token_;
        proxy = proxy_;
    }

    if (token.empty() || wsUrl.empty()) {
        return false;
    }

    // Parse WebSocket URL
    bool useSSL = (wsUrl.find("wss://") == 0);
    std::string hostPath = wsUrl.substr(useSSL ? 6 : 5); // skip "wss://" or "ws://"
    std::string host, path;
    INTERNET_PORT port = useSSL ? 443 : 80;

    auto slashPos = hostPath.find('/');
    if (slashPos != std::string::npos) {
        host = hostPath.substr(0, slashPos);
        path = hostPath.substr(slashPos);
    } else {
        host = hostPath;
        path = "/";
    }

    auto colonPos = host.find(':');
    if (colonPos != std::string::npos) {
        port = static_cast<INTERNET_PORT>(std::stoi(host.substr(colonPos + 1)));
        host = host.substr(0, colonPos);
    }

    // Open WinHTTP session — honour user-configured HTTP proxy if any.
    // Same pattern as HttpClient::DoRequest. Empty `proxy` falls back to
    // the system / IE default. proxyW must outlive WinHttpOpen.
    HINTERNET hSession;
    std::wstring proxyW;
    if (!proxy.empty()) {
        proxyW = Utf8ToWide(proxy);
        hSession = WinHttpOpen(L"StickyTodo-WS/1.0",
                               WINHTTP_ACCESS_TYPE_NAMED_PROXY,
                               proxyW.c_str(),
                               WINHTTP_NO_PROXY_BYPASS, 0);
    } else {
        hSession = WinHttpOpen(L"StickyTodo-WS/1.0",
                               WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                               WINHTTP_NO_PROXY_NAME,
                               WINHTTP_NO_PROXY_BYPASS, 0);
    }
    if (!hSession) return false;

    HINTERNET hConnect = WinHttpConnect(hSession, Utf8ToWide(host).c_str(), port, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return false;
    }

    DWORD flags = useSSL ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET",
                                             Utf8ToWide(path).c_str(),
                                             nullptr, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    // Upgrade to WebSocket
    if (!WinHttpSetOption(hRequest, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0)) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                             WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    if (!WinHttpReceiveResponse(hRequest, nullptr)) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    HINTERNET hWebSocket = WinHttpWebSocketCompleteUpgrade(hRequest, 0);
    WinHttpCloseHandle(hRequest); // No longer needed after upgrade

    if (!hWebSocket) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    // Publish the live handle so Disconnect() on another thread can
    // force-unblock ReceiveLoop by closing it. Ownership of the handle
    // is now shared between this thread and Disconnect via
    // liveWebSocket_.exchange(nullptr) — whichever thread's exchange
    // returns the non-null value is responsible for WinHttpCloseHandle.
    liveWebSocket_.store(hWebSocket);

    // Send auth frame: {"type":"auth","token":"<jwt>"}. If the WinHTTP
    // stack refuses the send (e.g. TCP reset mid-handshake), bail out
    // now rather than wasting the subsequent ReceiveLoop call: the
    // server enforces a 2-second auth deadline (AGENTS.md §3.3) so any
    // delay here is already risking a 4401 close.
    unsigned long sendErr = SendAuthFrame(hWebSocket);
    if (sendErr != ERROR_SUCCESS) {
        // Take back ownership of the handle — Disconnect may have raced
        // us. If we win the exchange, close here; if Disconnect won, it
        // has already closed (or will), so we skip.
        if (void* owned = liveWebSocket_.exchange(nullptr)) {
            WinHttpCloseHandle(static_cast<HINTERNET>(owned));
        }
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    // Receive loop (blocking until disconnect, error, or Disconnect()
    // force-closes the handle).
    ReceiveLoop(hWebSocket);

    // Normal return path: try to take back ownership. If Disconnect
    // already force-closed the handle, exchange returns nullptr and we
    // skip the redundant (UB-inducing) double close.
    if (void* owned = liveWebSocket_.exchange(nullptr)) {
        // Attempt a graceful close frame first; ignore failure (handle
        // may already be in a terminal state after the server closed).
        WinHttpWebSocketClose(static_cast<HINTERNET>(owned),
                               WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS,
                               nullptr, 0);
        WinHttpCloseHandle(static_cast<HINTERNET>(owned));
    }
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    return true;
}

unsigned long WebSocketClient::SendAuthFrame(void* hWebSocket) {
    std::string token;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        token = token_;
    }

    nlohmann::json authFrame;
    authFrame["type"] = "auth";
    authFrame["token"] = token;
    std::string payload = authFrame.dump();

    // Return value is a Win32 error code per WinHTTP docs: ERROR_SUCCESS
    // on success (frame queued), otherwise an error. The server-side
    // 2-second auth deadline (AGENTS.md §3.3) means we must surface
    // send failures immediately rather than soldier on into ReceiveLoop.
    return WinHttpWebSocketSend(static_cast<HINTERNET>(hWebSocket),
                                 WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                                 const_cast<char*>(payload.data()),
                                 static_cast<DWORD>(payload.size()));
}

void WebSocketClient::ReceiveLoop(void* hWebSocket) {
    HINTERNET ws = static_cast<HINTERNET>(hWebSocket);
    std::vector<char> buffer(4096);
    std::string accumulated;

    // No client-side ping timer. See WebSocketClient.h header comment:
    // server sends 30 s pings, WinHTTP auto-pongs, pongWait=60 s covers us.
    while (shouldRun_.load()) {
        DWORD bytesRead = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE bufType;

        // Blocking receive. The only ways this returns:
        //   1) A frame (or frame fragment) arrived — err == ERROR_SUCCESS.
        //   2) Server initiated close — bufType ==
        //      WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE, handled below.
        //   3) Disconnect() force-closed the handle on another thread —
        //      err == ERROR_WINHTTP_OPERATION_CANCELLED, so we fall out.
        //   4) Any other transport error — err != ERROR_SUCCESS, we fall
        //      out and let WorkerLoop drive exponential-backoff reconnect.
        DWORD err = WinHttpWebSocketReceive(ws, buffer.data(),
                                             static_cast<DWORD>(buffer.size()),
                                             &bytesRead, &bufType);
        if (err != ERROR_SUCCESS) {
            break;
        }

        if (bufType == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
            // Server closed connection
            USHORT closeStatus = 0;
            DWORD closeReasonLen = 0;
            char closeReason[256] = {};
            WinHttpWebSocketQueryCloseStatus(ws, &closeStatus, closeReason,
                                              sizeof(closeReason), &closeReasonLen);

            if (closeStatus == kCloseCodeUnauthorized) {
                // 4401 = Unauthorized → do NOT reconnect
                shouldRun_ = false;
                connected_ = false;
                if (onSignal_) onSignal_(WsSignal::Unauthorized);
                return;
            }
            break;
        }

        accumulated.append(buffer.data(), bytesRead);

        // Complete message?
        if (bufType == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) {
            // Parse the message
            try {
                auto j = nlohmann::json::parse(accumulated);
                std::string type = j.value("type", "");

                if (type == "ready") {
                    // Auth succeeded
                    connected_ = true;
                    reconnectAttempt_ = 0;

                    if (hadSuccessfulConnection_) {
                        if (onSignal_) onSignal_(WsSignal::Reconnected);
                    } else {
                        hadSuccessfulConnection_ = true;
                        if (onSignal_) onSignal_(WsSignal::Ready);
                    }
                } else if (!type.empty()) {
                    // Business event
                    WsEvent event;
                    event.type = type;
                    if (j.contains("data")) {
                        event.data = j["data"].dump();
                    }
                    if (j.contains("id")) {
                        if (j["id"].is_string()) {
                            event.id = j["id"].get<std::string>();
                        } else if (j["id"].is_number()) {
                            event.id = std::to_string(j["id"].get<uint64_t>());
                        }
                    }
                    if (onEvent_) onEvent_(event);
                }
            } catch (...) {
                // Malformed message — skip
            }
            accumulated.clear();
        }
        // No periodic upstream frames — see header comment on why we MUST
        // NOT send anything after the initial auth frame (server policy
        // rejects unsolicited upstream with close code 4400).
    }
}

} // namespace stickytodo::core
