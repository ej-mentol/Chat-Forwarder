#ifndef PLUGINS_H
#define PLUGINS_H

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <metahook.h>
#include "interface.h"
#include "HLSDK/common/cvardef.h"

// Steam API - for local player SteamID64
#include <steam_api.h>

#include <queue>
#include <string>
#include <thread>
#include <mutex>
#include <algorithm>
#include <atomic>
#include <memory>
#include <condition_variable>
#include <cstdint>

constexpr size_t MAX_QUEUE_SIZE   = 1000;
constexpr int DEFAULT_LISTEN_PORT = 26001;
constexpr int DEFAULT_SERVER_PORT = 26000;
constexpr int SOCKET_TIMEOUT_MS   = 500;

// ---------------------------------------------------------------------------
// UDP packet layout (all message types, all 5 tags):
//
//   [0]     type_byte  : uint8   - message type tag (0x12-0x16)
//   [1..8]  steamid64  : uint64  - local player SteamID64, little-endian
//                                  0 = Steam unavailable / not logged in (LAN)
//   [9..]   message    : char[]  - message text, NOT null-terminated
//                                  use msglen for exact byte count
//
// Parser (Python):
//   tag     = buf[0]
//   steamid = struct.unpack_from('<Q', buf, 1)[0]   # 0 = no Steam / LAN
//   text    = buf[9:].decode('utf-8', errors='replace')
// ---------------------------------------------------------------------------

// Message type tags
constexpr uint8_t MSG_TYPE_CHAT  = 0x12;
constexpr uint8_t MSG_TYPE_GAME  = 0x13;
constexpr uint8_t MSG_TYPE_NET   = 0x14;
constexpr uint8_t MSG_TYPE_SYS   = 0x15;
constexpr uint8_t MSG_TYPE_STUFF = 0x16;

// Fixed header size: 1 (tag) + 8 (steamid64 LE) = 9 bytes
constexpr size_t CF_HEADER_SIZE = 9;
// Maximum text payload: keep total UDP packet under 1024 bytes
constexpr size_t CF_MAX_TEXT    = 1024 - CF_HEADER_SIZE;

// ---------------------------------------------------------------------------
// SendTask: queued unit of work for the sender thread.
// The sender thread assembles the binary packet from these fields.
// ---------------------------------------------------------------------------
struct SendTask {
    uint8_t  tag;               // message type (MSG_TYPE_CHAT etc.)
    uint64_t steamid;           // SteamID64 LE; 0 = not available (LAN / no Steam)
    char     text[CF_MAX_TEXT]; // message text, not null-terminated beyond msglen
    uint16_t msglen;            // actual byte count in text[]
    char     server_ip[64];
    int      port;
};

// ---------------------------------------------------------------------------
// Returns the local player's SteamID64.
// Returns 0 if Steam interface is unavailable or player is not logged on.
// Only call from the main thread (user message callbacks, HUD_Init, HUD_Frame).
// ---------------------------------------------------------------------------
inline uint64_t GetLocalSteamID64() {
    auto* pSteamUser = SteamUser();
    if (!pSteamUser || !pSteamUser->BLoggedOn())
        return 0;
    return pSteamUser->GetSteamID().ConvertToUint64();
}

// ---------------------------------------------------------------------------
// Classes
// ---------------------------------------------------------------------------
class SendQueue {
public:
    bool push(SendTask task) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.size() >= MAX_QUEUE_SIZE || shutdown_) {
            static size_t dropCount = 0;
            if (++dropCount % 100 == 0) {
                OutputDebugStringA("[ChatForwarder] SendQueue overflow! Dropped 100 messages.\n");
            }
            return false;
        }
        queue_.push(std::move(task));
        cv_.notify_one();
        return true;
    }

    bool pop(SendTask& task, int timeout_ms = 100) {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
            [this] { return !queue_.empty() || shutdown_; });

        if (queue_.empty() || shutdown_) {
            return false;
        }

        task = std::move(queue_.front());
        queue_.pop();
        return true;
    }

    void shutdown() {
        std::lock_guard<std::mutex> lock(mutex_);
        shutdown_ = true;
        cv_.notify_all();
    }

    void reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::queue<SendTask> empty;
        queue_.swap(empty);
        shutdown_ = false;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::queue<SendTask> empty;
        queue_.swap(empty);
    }

private:
    std::queue<SendTask> queue_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> shutdown_{ false };
};

class MessageQueue {
public:
    bool push(std::string msg) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.size() >= MAX_QUEUE_SIZE || shutdown_) {
            return false;
        }
        queue_.push(std::move(msg));
        cv_.notify_one();
        return true;
    }
    bool pop(std::string& msg, int timeout_ms = 0) {
        std::unique_lock<std::mutex> lock(mutex_);

        if (timeout_ms > 0) {
            cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                [this] { return !queue_.empty() || shutdown_; });
        }

        if (queue_.empty() || shutdown_) {
            return false;
        }

        msg = std::move(queue_.front());
        queue_.pop();
        return true;
    }
    void shutdown() {
        std::lock_guard<std::mutex> lock(mutex_);
        shutdown_ = true;
        cv_.notify_all();
    }
    void reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::queue<std::string> empty;
        queue_.swap(empty);
        shutdown_ = false;
    }
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::queue<std::string> empty;
        queue_.swap(empty);
    }
    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }
private:
    std::queue<std::string> queue_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> shutdown_{ false };
};

class WinsockRAII {
public:
    WinsockRAII() : initialized_(false) {
        WSADATA wsaData;
        int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
        if (result == 0) {
            if (LOBYTE(wsaData.wVersion) == 2 && HIBYTE(wsaData.wVersion) == 2) {
                initialized_ = true;
            }
            else {
                WSACleanup();
            }
        }
    }
    ~WinsockRAII() {
        if (initialized_) {
            WSACleanup();
        }
    }
    bool IsInitialized() const { return initialized_; }

    WinsockRAII(const WinsockRAII&) = delete;
    WinsockRAII& operator=(const WinsockRAII&) = delete;

    WinsockRAII(WinsockRAII&& other) noexcept : initialized_(other.initialized_) {
        other.initialized_ = false;
    }

    WinsockRAII& operator=(WinsockRAII&& other) noexcept {
        if (this != &other) {
            if (initialized_) {
                WSACleanup();
            }
            initialized_ = other.initialized_;
            other.initialized_ = false;
        }
        return *this;
    }
private:
    bool initialized_;
};

// ---------------------------------------------------------------------------
// Externs
// ---------------------------------------------------------------------------
extern cl_enginefunc_t gEngfuncs;
extern cl_exportfuncs_t gExportfuncs;
extern metahook_api_t* g_pMetaHookAPI;

extern MessageQueue g_messageQueue;
extern SendQueue g_sendQueue;

extern cvar_t* cf_server_ip;
extern cvar_t* cf_server_port;
extern cvar_t* cf_listen_port;
extern cvar_t* cf_enabled;
extern cvar_t* cf_debug;
extern cvar_t* cf_listen_only;
extern cvar_t* cf_command_delay;

extern std::chrono::steady_clock::time_point g_lastCommandTime;
extern void (*g_pfnHUD_Init)(void);
extern void (*g_pfnHUD_Frame)(double time);

extern ThreadPoolHandle_t g_hThreadPool;
extern ThreadWorkItemHandle_t g_hListenerWorkItem;
extern ThreadWorkItemHandle_t g_hSenderWorkItem;

extern std::atomic<bool> g_shutdownListener;
extern std::atomic<bool> g_shutdownSender;

extern std::unique_ptr<WinsockRAII> g_winsock;
extern hook_t* g_hOutputDebugStringHook;
extern bool g_bChatForwarderInitialized;
extern pfnUserMsgHook g_pfnTextMsg;
extern void (WINAPI* g_pfnOutputDebugStringA)(LPCSTR lpOutputString);
extern fn_parsefunc g_pfnCL_ParsePrint;
extern fn_parsefunc g_pfnCL_ParseStuffText;

// ---------------------------------------------------------------------------
// Functions
// ---------------------------------------------------------------------------
void HUD_Init(void);
void HUD_Frame(double time);
void ChatForwarder_Init(void);
int __MsgFunc_SayText(const char* pszName, int iSize, void* pbuf);
int __MsgFunc_TextMsg(const char* pszName, int iSize, void* pbuf);
bool UDPListenerWorkCallback(void* ctx);
bool SenderWorkCallback(void* ctx);
std::string CleanMessage(const char* input);

// Enqueues a message for UDP delivery.
// steamid: only pass GetLocalSteamID64() for MSG_TYPE_CHAT (local player chat).
//          Pass 0 (default) for all other types — server/game/system messages
//          do not have a meaningful local SteamID.
// All 5 message types use this single function.
void QueueTask(uint8_t tag, const std::string& msg, uint64_t steamid = 0);

inline bool IsCvarValid(const cvar_t* cvar) {
    return cvar && cvar->string && cvar->string[0] != '\0';
}

#endif // PLUGINS_H
