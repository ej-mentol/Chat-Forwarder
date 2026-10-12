// plugins.cpp
#include "plugins.h"
#include "netdiag.h"
#include "Interface/IPlugins.h"
#include <chrono>
#include <cctype>
#pragma comment(lib, "ws2_32.lib")
cl_enginefunc_t gEngfuncs;
cl_exportfuncs_t gExportfuncs;
metahook_api_t* g_pMetaHookAPI = NULL;
mh_interface_t* g_pInterface = NULL;
mh_enginesave_t* g_pMetaSave = NULL;

MessageQueue g_messageQueue;
NetConfig g_netConfig;
cvar_t* cf_server_ip = NULL;
cvar_t* cf_server_port = NULL;
cvar_t* cf_allow_ip = NULL;
cvar_t* cf_listen_port = NULL;
cvar_t* cf_enabled = NULL;
cvar_t* cf_debug = NULL;
cvar_t* cf_listen_only = NULL;
cvar_t* cf_command_delay = NULL;

std::chrono::steady_clock::time_point g_lastCommandTime;
void (*g_pfnHUD_Init)(void) = NULL;
void (*g_pfnHUD_Frame)(double time) = NULL;
int (*g_pfnHUD_GetStudioModelInterface)(int version, struct r_studio_interface_s **ppinterface, struct engine_studio_api_s *pstudio) = NULL;
engine_studio_api_t g_engineStudio = { 0 };
ThreadPoolHandle_t g_hThreadPool = nullptr;
ThreadWorkItemHandle_t g_hListenerWorkItem = nullptr;
std::atomic<bool> g_shutdownListener(false);
std::unique_ptr<WinsockRAII> g_winsock = nullptr;
SendQueue g_sendQueue;
ThreadWorkItemHandle_t g_hSenderWorkItem = nullptr;
std::atomic<bool> g_shutdownSender(false);
pfnUserMsgHook g_pfnTextMsg = NULL;
hook_t* g_hOutputDebugStringHook = nullptr;
bool g_bChatForwarderInitialized = false;

void (WINAPI* g_pfnOutputDebugStringA)(LPCSTR lpOutputString) = NULL;

void WINAPI NewOutputDebugStringA(LPCSTR lpOutputString) {
    static thread_local bool g_inHook = false;
    if (g_inHook || !lpOutputString || !lpOutputString[0]) {
        if (g_pfnOutputDebugStringA) g_pfnOutputDebugStringA(lpOutputString);
        return;
    }

    struct ReentryGuard {
        bool& flag;
        explicit ReentryGuard(bool& value) : flag(value) { flag = true; }
        ~ReentryGuard() { flag = false; }
    } guard(g_inHook);

    if (g_netConfig.enabled.load(std::memory_order_relaxed)) {
        static std::string g_sysLogBuffer;
        static std::mutex g_logMutex;

        std::lock_guard<std::mutex> lock(g_logMutex);
        g_sysLogBuffer += lpOutputString;

        size_t pos;
        while ((pos = g_sysLogBuffer.find('\n')) != std::string::npos) {
            std::string line = g_sysLogBuffer.substr(0, pos);
            // Remove \r if present at the end
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }

            g_sysLogBuffer.erase(0, pos + 1);

            std::string cleanMsg = CleanMessage(line.c_str());
            if (!cleanMsg.empty()) {
                QueueTask(MSG_TYPE_SYS, cleanMsg);
            }
        }

        // Safety valve: if buffer grows too large without newline, flush it
        if (g_sysLogBuffer.size() > 4096) {
            std::string cleanMsg = CleanMessage(g_sysLogBuffer.c_str());
            if (!cleanMsg.empty()) {
                QueueTask(MSG_TYPE_SYS, cleanMsg);
            }
            g_sysLogBuffer.clear();
        }
    }

    if (g_pfnOutputDebugStringA) g_pfnOutputDebugStringA(lpOutputString);
}

bool UDPListenerWorkCallback(void* ctx)
{
    NetDiag& d = Diag();
    SOCKET sock = INVALID_SOCKET;
    uint32_t gen = 0;
    std::string lastDeniedIp;
    char buffer[512];

    while (!g_shutdownListener.load(std::memory_order_relaxed))
    {
        if (!g_netConfig.enabled.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        NetCfg cfg;
        uint32_t g = CfgGet(cfg);
        if (g == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        if (sock == INVALID_SOCKET || g != gen) {
            if (sock != INVALID_SOCKET) closesocket(sock);
            std::string info;
            sock = OpenUdp(cfg.bindIp, cfg.listenPort, true, SOCKET_TIMEOUT_MS, info);
            SetState(&NetDiag::listenState, "listener", info, sock != INVALID_SOCKET);
            gen = g;
            if (sock == INVALID_SOCKET) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                continue;
            }
        }

        sockaddr_in from = {};
        int fromLen = sizeof(from);
        int n = recvfrom(sock, buffer, sizeof(buffer), 0, (sockaddr*)&from, &fromLen);
        if (n == SOCKET_ERROR) {
            int e = WSAGetLastError();
            if (e == WSAETIMEDOUT || e == WSAEWOULDBLOCK) continue;
            if (e == WSAECONNRESET || e == WSAEMSGSIZE) {
                DiagLog(1, "listener: recv WSA %d ignored", e);
                continue;
            }
            closesocket(sock);
            sock = INVALID_SOCKET;
            SetState(&NetDiag::listenState, "listener", WsaErr("recvfrom", e) + ", rebinding", false);
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }
        if (n <= 0) continue;

        ++d.rx;
        d.lastRxMs = NowMs();
        const std::string fromStr = AddrStr(from);

        if (!cfg.allowIp.empty() && cfg.allowIp != "*") {
            in_addr allowed;
            if (!ParseIp(cfg.allowIp, allowed) || from.sin_addr.s_addr != allowed.s_addr) {
                ++d.rxDenied;
                std::string ip = fromStr.substr(0, fromStr.rfind(':'));
                if (ip != lastDeniedIp) {
                    lastDeniedIp = ip;
                    DiagLog(0, "listener: DROPPED packet from %s (cf_allow_ip=%s)", ip.c_str(), cfg.allowIp.c_str());
                }
                continue;
            }
        }

        {
            std::lock_guard<std::mutex> lk(d.mtx);
            d.lastRxFrom = fromStr;
        }

        // Pong packet check: 0x1F ('\x1F') followed by PONG
        if ((unsigned char)buffer[0] == CF_PONG_MARK) {
            std::string pongMsg(buffer, n);
            NetDiag_HandlePong(pongMsg);
            continue;
        }

        std::string msg(buffer, n);
        // Trim trailing whitespace and control characters
        while (!msg.empty() && (unsigned char)msg.back() <= 32) {
            msg.pop_back();
        }
        // Trim leading whitespace
        size_t first = msg.find_first_not_of(" \t\n\r");
        if (first != std::string::npos && first > 0) {
            msg = msg.substr(first);
        }
        if (msg.empty()) continue;

        {
            std::lock_guard<std::mutex> lk(d.mtx);
            d.lastRxHead = (msg.size() > 32) ? msg.substr(0, 32) : msg;
        }

        if (cfg.sayOnly) {
            auto startsWithIgnoreCase = [](const std::string& str, const char* prefix, size_t prefixLen) {
                if (str.size() < prefixLen) return false;
                for (size_t i = 0; i < prefixLen; ++i) {
                    if (tolower((unsigned char)str[i]) != tolower((unsigned char)prefix[i]))
                        return false;
                }
                return true;
            };

            bool say = startsWithIgnoreCase(msg, "say ", 4) || startsWithIgnoreCase(msg, "say_team ", 9);
            if (!say || msg.find_first_of(";\n\r\0", 0, 4) != std::string::npos) {
                ++d.rxFiltered;
                continue;
            }
        }

        ++d.rxCmd;
        g_messageQueue.push(std::move(msg));
    }

    if (sock != INVALID_SOCKET) closesocket(sock);
    return true;
}

bool SenderWorkCallback(void* ctx) {
    NetDiag& d = Diag();
    SOCKET sock = INVALID_SOCKET;
    uint32_t gen = 0;
    bool failing = false;
    std::string okInfo;
    char pktbuf[CF_HEADER_SIZE + CF_MAX_TEXT];

    while (!g_shutdownSender.load(std::memory_order_relaxed)) {
        NetCfg cfg;
        uint32_t g = CfgGet(cfg);
        if (g == 0 || !g_netConfig.enabled.load(std::memory_order_relaxed) || cfg.listenOnly) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        if (sock == INVALID_SOCKET || g != gen) {
            if (sock != INVALID_SOCKET) closesocket(sock);
            sock = OpenUdp(cfg.bindIp, 0, false, 0, okInfo);
            SetState(&NetDiag::senderState, "sender", okInfo, sock != INVALID_SOCKET);
            gen = g;
            failing = false;
            if (sock == INVALID_SOCKET) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                continue;
            }
        }

        SendTask task;
        if (g_sendQueue.pop(task, 1)) {
            do {
                sockaddr_in addr = {};
                addr.sin_family = AF_INET;
                addr.sin_port = htons(static_cast<u_short>(task.port));
                if (inet_pton(AF_INET, task.server_ip, &addr.sin_addr) != 1) {
                    continue;
                }

                const size_t pktlen = CF_HEADER_SIZE + task.msglen;
                pktbuf[0] = static_cast<char>(task.tag);
                uint64_t sid = task.steamid;
                for (int i = 0; i < 8; ++i) {
                    pktbuf[1 + i] = static_cast<char>(sid & 0xFFu);
                    sid >>= 8;
                }
                memcpy(pktbuf + CF_HEADER_SIZE, task.text, task.msglen);

                int rc = sendto(sock, pktbuf, static_cast<int>(pktlen), 0,
                    (const sockaddr*)&addr, sizeof(addr));
                if (rc == SOCKET_ERROR) {
                    int e = WSAGetLastError();
                    ++d.txErr;
                    d.lastTxErr = e;
                    failing = true;
                    SetState(&NetDiag::senderState, "sender",
                        WsaErr((std::string("sendto ") + task.server_ip).c_str(), e), false);
                } else {
                    ++d.tx;
                    d.lastTxMs = NowMs();
                    if (failing) {
                        failing = false;
                        SetState(&NetDiag::senderState, "sender", okInfo, true);
                    }
                }
            } while (g_sendQueue.pop(task, 0));
        }
    }

    if (sock != INVALID_SOCKET) closesocket(sock);
    return true;
}

void CleanupResources()
{
    // 1. Signal all worker threads to stop
    g_shutdownListener.store(true, std::memory_order_release);
    g_shutdownSender.store(true, std::memory_order_release);

    // 2. Wake up threads blocked on condition_variable so they exit immediately
    //    instead of waiting for the next pop/push timeout
    g_sendQueue.shutdown();
    g_messageQueue.shutdown();

    // 3. Wait for listener thread to finish, then release its MetaHook work item
    if (g_hListenerWorkItem) {
        if (g_pMetaHookAPI && g_hThreadPool) {
            g_pMetaHookAPI->WaitForWorkItemToComplete(g_hListenerWorkItem);
            // g_pMetaHookAPI->DeleteWorkItem(g_hListenerWorkItem);
        }
        g_hListenerWorkItem = nullptr;
    }

    // 4. Wait for sender thread to finish, then release its MetaHook work item
    if (g_hSenderWorkItem) {
        if (g_pMetaHookAPI && g_hThreadPool) {
            g_pMetaHookAPI->WaitForWorkItemToComplete(g_hSenderWorkItem);
            // g_pMetaHookAPI->DeleteWorkItem(g_hSenderWorkItem);
        }
        g_hSenderWorkItem = nullptr;
    }

    // 5. Remove the IAT hook before the plugin can be unloaded.
    if (g_hOutputDebugStringHook && g_pMetaHookAPI) {
        g_pMetaHookAPI->UnHook(g_hOutputDebugStringHook);
        g_hOutputDebugStringHook = nullptr;
        g_pfnOutputDebugStringA = nullptr;
    }

    // 6. Release Winsock and allow clean reinitialization on the next HUD_Init.
    g_winsock.reset();
    g_sendQueue.reset();
    g_messageQueue.reset();
    g_bChatForwarderInitialized = false;
}
void ChatForwarder_Init(void)
{
    if (!g_pMetaHookAPI) return;

    // Engine type check as suggested
    auto engineType = g_pMetaHookAPI->GetEngineType();
    if (engineType == ENGINE_GOLDSRC_BLOB) {
        // Blob engines can have limitations, check if we can get the client base.
        if (g_pMetaHookAPI->GetClientBase() == 0) {
            g_pMetaHookAPI->SysError("ChatForwarder: Blob client is not supported by this plugin.");
            return;
        }
    }

    // Initialize global resources once
    if (!g_bChatForwarderInitialized)
    {
        // Optional: Check for debugger
        if (g_pMetaHookAPI->IsDebuggerPresent()) {
            gEngfuncs.Con_Printf("[CF] Debugger detected.\n");
        }

        g_winsock = std::make_unique<WinsockRAII>();
        if (!g_winsock->IsInitialized()) {
            g_pMetaHookAPI->SysError("Failed to initialize Winsock");
            return;
        }

        if (gEngfuncs.pfnRegisterVariable) {
            cf_server_ip = gEngfuncs.pfnRegisterVariable("cf_server_ip", "127.0.0.1", FCVAR_ARCHIVE);
            cf_server_port = gEngfuncs.pfnRegisterVariable("cf_server_port", "26000", FCVAR_ARCHIVE);
            cf_allow_ip = gEngfuncs.pfnRegisterVariable("cf_allow_ip", "*", FCVAR_ARCHIVE);
            cf_listen_port = gEngfuncs.pfnRegisterVariable("cf_listen_port", "26001", FCVAR_ARCHIVE);
            cf_enabled = gEngfuncs.pfnRegisterVariable("cf_enabled", "1", FCVAR_ARCHIVE);
            cf_debug = gEngfuncs.pfnRegisterVariable("cf_debug", "0", FCVAR_ARCHIVE);
            cf_listen_only = gEngfuncs.pfnRegisterVariable("cf_listen_only", "0", FCVAR_ARCHIVE);
            cf_command_delay = gEngfuncs.pfnRegisterVariable("cf_command_delay", "0", FCVAR_ARCHIVE);
        }

        NetDiag_Init();

        // Hook OutputDebugStringA in engine to capture everything DebugView sees
        // Only hook once!
        g_hOutputDebugStringHook = g_pMetaHookAPI->IATHook(g_pMetaHookAPI->GetEngineModule(), "kernel32.dll", "OutputDebugStringA", NewOutputDebugStringA, (void**)&g_pfnOutputDebugStringA);

        g_bChatForwarderInitialized = true;
    }

    // Ensure thread pool is valid
    g_hThreadPool = g_pMetaHookAPI->GetGlobalThreadPool();
    if (!g_hThreadPool) {
        g_pMetaHookAPI->SysError("Failed to get MetaHook thread pool");
        return;
    }

    // Always start Sender thread.
    // Dynamic gating for cf_listen_only and cf_enabled is handled inside SenderWorkCallback.
    if (!g_hSenderWorkItem) {
        g_shutdownSender.store(false, std::memory_order_relaxed);
        g_hSenderWorkItem = g_pMetaHookAPI->CreateWorkItem(g_hThreadPool, SenderWorkCallback, nullptr);

        if (!g_hSenderWorkItem) {
            g_pMetaHookAPI->SysError("Failed to create sender work item");
            return;
        }
        g_pMetaHookAPI->QueueWorkItem(g_hThreadPool, g_hSenderWorkItem);
    }

    // Always start the Listener thread — it handles inbound console commands
    // regardless of cf_listen_only mode.
    if (!g_hListenerWorkItem) {
        g_shutdownListener.store(false, std::memory_order_relaxed);
        g_hListenerWorkItem = g_pMetaHookAPI->CreateWorkItem(g_hThreadPool, UDPListenerWorkCallback, nullptr);

        if (!g_hListenerWorkItem) {
            g_pMetaHookAPI->SysError("Failed to create listener work item");
            return;
        }
        g_pMetaHookAPI->QueueWorkItem(g_hThreadPool, g_hListenerWorkItem);
    }
}
void IPluginsV4::Init(metahook_api_t* pAPI, mh_interface_t* pInterface, mh_enginesave_t* pSave)
{
    g_pInterface = pInterface;
    g_pMetaHookAPI = pAPI;
    g_pMetaSave = pSave;
}
void IPluginsV4::Shutdown(void)
{
    CleanupResources();
}
void IPluginsV4::LoadEngine(cl_enginefunc_t* pEngfuncs)
{
    memcpy(&gEngfuncs, pEngfuncs, sizeof(gEngfuncs));
}
void IPluginsV4::LoadClient(cl_exportfuncs_t* pExportFunc)
{
    if (!pExportFunc) return;
    memcpy(&gExportfuncs, pExportFunc, sizeof(gExportfuncs));
    g_pfnHUD_Init = pExportFunc->HUD_Init;
    pExportFunc->HUD_Init = HUD_Init;
    g_pfnHUD_Frame = pExportFunc->HUD_Frame;
    pExportFunc->HUD_Frame = HUD_Frame;
    g_pfnHUD_GetStudioModelInterface = pExportFunc->HUD_GetStudioModelInterface;
    if (g_pfnHUD_GetStudioModelInterface) {
        pExportFunc->HUD_GetStudioModelInterface = HUD_GetStudioModelInterface;
    }
}
void IPluginsV4::ExitGame(int iResult)
{
    CleanupResources();
}
const char* IPluginsV4::GetVersion(void)
{
    return "1.5.0.0";
}
EXPOSE_SINGLE_INTERFACE(IPluginsV4, IPluginsV4, METAHOOK_PLUGIN_API_VERSION_V4);
