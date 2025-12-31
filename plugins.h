#ifndef PLUGINS_H
#define PLUGINS_H

#include <winsock2.h>
#include <ws2tcpip.h>
#include <metahook.h>
#include "interface.h"
#include "HLSDK/common/cvardef.h"

#include <queue>
#include <string>
#include <thread>
#include <mutex>
#include <algorithm>
#include <atomic>
#include <memory>
#include <condition_variable>

constexpr size_t MAX_COMMAND_SIZE = 275;
constexpr size_t MAX_QUEUE_SIZE = 1000;
constexpr int DEFAULT_LISTEN_PORT = 26001;
constexpr int DEFAULT_SERVER_PORT = 26000;
constexpr int SOCKET_TIMEOUT_MS = 500;
constexpr int THREAD_JOIN_TIMEOUT_MS = 2000;

// Structs
struct SenderWorkContext {
    char message[1024];
    char server_ip[256];
    char server_port[16];
};

struct SendTask {
    char message[1024];
    char server_ip[256];
    int port;
};

// Classes
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

// Externs
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
// extern cvar_t* cf_capture_mode; // Removed in favor of client-side filtering

// Message Source Tags
constexpr char MSG_TYPE_CHAT  = '\x12';
constexpr char MSG_TYPE_GAME  = '\x13';
constexpr char MSG_TYPE_NET   = '\x14';
constexpr char MSG_TYPE_SYS   = '\x15';
constexpr char MSG_TYPE_STUFF = '\x16';

extern std::chrono::steady_clock::time_point g_lastCommandTime;
extern void (*g_pfnHUD_Init)(void);
extern void (*g_pfnHUD_Frame)(double time);

extern ThreadPoolHandle_t g_hThreadPool;
extern ThreadWorkItemHandle_t g_hListenerWorkItem;
extern ThreadWorkItemHandle_t g_hSenderWorkItem;

extern std::atomic<bool> g_shutdownListener;
extern std::atomic<bool> g_shutdownSender;

extern std::unique_ptr<WinsockRAII> g_winsock;
extern pfnUserMsgHook g_pfnTextMsg;
extern void (WINAPI* g_pfnOutputDebugStringA)(LPCSTR lpOutputString);
extern fn_parsefunc g_pfnCL_ParsePrint;
extern fn_parsefunc g_pfnCL_ParseStuffText;

// Functions
void HUD_Init(void);
void HUD_Frame(double time);
void ChatForwarder_Init(void);
int __MsgFunc_SayText(const char* pszName, int iSize, void* pbuf);
int __MsgFunc_TextMsg(const char* pszName, int iSize, void* pbuf);
bool UDPListenerWorkCallback(void* ctx);
bool SenderWorkCallback(void* ctx);
std::string CleanMessage(const char* input);

inline bool IsCvarValid(const cvar_t* cvar) {
    return cvar && cvar->string && cvar->string[0] != '\0';
}

#endif // PLUGINS_H