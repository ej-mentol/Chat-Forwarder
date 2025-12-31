// plugins.cpp
#include "plugins.h"
#include "Interface/IPlugins.h"
#include <chrono>
#pragma comment(lib, "ws2_32.lib")
cl_enginefunc_t gEngfuncs;
cl_exportfuncs_t gExportfuncs;
metahook_api_t* g_pMetaHookAPI = NULL;
mh_interface_t* g_pInterface = NULL;
mh_enginesave_t* g_pMetaSave = NULL;

MessageQueue g_messageQueue;
cvar_t* cf_server_ip = NULL;
cvar_t* cf_server_port = NULL;
cvar_t* cf_listen_port = NULL;
cvar_t* cf_enabled = NULL;
cvar_t* cf_debug = NULL;
cvar_t* cf_listen_only = NULL;
cvar_t* cf_command_delay = NULL;

std::chrono::steady_clock::time_point g_lastCommandTime;
void (*g_pfnHUD_Init)(void) = NULL;
void (*g_pfnHUD_Frame)(double time) = NULL;
ThreadPoolHandle_t g_hThreadPool = nullptr;
ThreadWorkItemHandle_t g_hListenerWorkItem = nullptr;
std::atomic<bool> g_shutdownListener(false);
std::unique_ptr<WinsockRAII> g_winsock = nullptr;
SendQueue g_sendQueue;
ThreadWorkItemHandle_t g_hSenderWorkItem = nullptr;
std::atomic<bool> g_shutdownSender(false);
pfnUserMsgHook g_pfnTextMsg = NULL;

void (WINAPI* g_pfnOutputDebugStringA)(LPCSTR lpOutputString) = NULL;

void WINAPI NewOutputDebugStringA(LPCSTR lpOutputString) {
    static thread_local bool g_inHook = false;
    if (g_inHook || !lpOutputString || !lpOutputString[0]) {
        if (g_pfnOutputDebugStringA) g_pfnOutputDebugStringA(lpOutputString);
        return;
    }

    g_inHook = true;

    if (IsCvarValid(cf_enabled) && atoi(cf_enabled->string) != 0) {
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
                std::string fullMsg;
                fullMsg += MSG_TYPE_SYS;
                fullMsg += cleanMsg;

                SendTask task;
                strncpy_s(task.message, fullMsg.c_str(), sizeof(task.message) - 1);
                strncpy_s(task.server_ip, cf_server_ip->string, sizeof(task.server_ip) - 1);
                task.port = atoi(cf_server_port->string);
                g_sendQueue.push(task);
            }
        }

        // Safety valve: if buffer grows too large without newline, flush it
        if (g_sysLogBuffer.size() > 4096) {
             std::string cleanMsg = CleanMessage(g_sysLogBuffer.c_str());
             if (!cleanMsg.empty()) {
                std::string fullMsg;
                fullMsg += MSG_TYPE_SYS;
                fullMsg += cleanMsg;

                SendTask task;
                strncpy_s(task.message, fullMsg.c_str(), sizeof(task.message) - 1);
                strncpy_s(task.server_ip, cf_server_ip->string, sizeof(task.server_ip) - 1);
                task.port = atoi(cf_server_port->string);
                g_sendQueue.push(task);
             }
             g_sysLogBuffer.clear();
        }
    }

    if (g_pfnOutputDebugStringA) g_pfnOutputDebugStringA(lpOutputString);
    g_inHook = false;
}

bool UDPListenerWorkCallback(void* ctx)
{
    SOCKET listenSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (listenSocket == INVALID_SOCKET) {
        return true;
    }
    struct SocketGuard {
        SOCKET sock;
        SocketGuard(SOCKET s) : sock(s) {}
        ~SocketGuard() {
            if (sock != INVALID_SOCKET) {
                closesocket(sock);
            }
        }
    } socketGuard(listenSocket);
    sockaddr_in serverAddr = {};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    int listenPort = DEFAULT_LISTEN_PORT;
    if (IsCvarValid(cf_listen_port)) {
        int port = atoi(cf_listen_port->string);
        if (port > 0 && port <= 65535) {
            listenPort = port;
        }
    }
    serverAddr.sin_port = htons(listenPort);
    {
        BOOL yes = TRUE;
        setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char*>(&yes), sizeof(yes));
    }
    if (bind(listenSocket, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        return true;
    }
    int timeout_ms = SOCKET_TIMEOUT_MS;
    setsockopt(listenSocket, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
    char buffer[256];
    while (!g_shutdownListener.load(std::memory_order_relaxed))
    {
        if (IsCvarValid(cf_enabled) && atoi(cf_enabled->string) == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        int bytesRead = recvfrom(listenSocket, buffer, sizeof(buffer), 0, NULL, NULL);
        if (bytesRead > 0)
        {
            std::string msg(buffer, bytesRead);

            // Trim all trailing control characters and spaces
            while (!msg.empty() && (unsigned char)msg.back() <= 32) {
                msg.pop_back();
            }
            // Trim leading spaces
            size_t first = msg.find_first_not_of(" \t\n\r");
            if (first != std::string::npos && first > 0) {
                msg = msg.substr(first);
            }

            if (!msg.empty()) {
                g_messageQueue.push(std::move(msg));
            }
        }
        else if (bytesRead == SOCKET_ERROR) {
            int error = WSAGetLastError();
            if (error != WSAETIMEDOUT && error != WSAEWOULDBLOCK) {
                break;
            }
        }
    }
    return true;
}

bool SenderWorkCallback(void* ctx) {
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        return true;
    }

    struct SocketGuard {
        SOCKET sock;
        SocketGuard(SOCKET s) : sock(s) {}
        ~SocketGuard() {
            if (sock != INVALID_SOCKET) {
                closesocket(sock);
            }
        }
    } socketGuard(sock);

    while (!g_shutdownSender.load(std::memory_order_relaxed)) {
        if (!IsCvarValid(cf_enabled) || atoi(cf_enabled->string) == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        SendTask task;
        // Batch processing: Drain the queue as fast as possible
        // Wait 1ms for the first item, then process remaining items instantly
        if (g_sendQueue.pop(task, 1)) {
            do {
                sockaddr_in addr = {};
                addr.sin_family = AF_INET;
                addr.sin_port = htons(task.port);
                inet_pton(AF_INET, task.server_ip, &addr.sin_addr);

                sendto(sock, task.message, strlen(task.message), 0,
                    (const sockaddr*)&addr, sizeof(addr));
            
            } while (g_sendQueue.pop(task, 0)); // Pop instantly until empty
        }
    }

    return true;
}

void CleanupResources()
{
    g_shutdownListener.store(true, std::memory_order_release);
    g_shutdownSender.store(true, std::memory_order_release);

    if (g_hListenerWorkItem) {
        g_hListenerWorkItem = nullptr;
    }

    if (g_hSenderWorkItem) {
        g_hSenderWorkItem = nullptr;
    }

    g_messageQueue.clear();
    g_sendQueue.clear();
    g_winsock.reset();
}
void ChatForwarder_Init(void)
{
    static bool bInitialized = false;

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
    if (!bInitialized)
    {
        // Optional: Check for debugger
        if (g_pMetaHookAPI->IsDebuggerPresent()) {
            gEngfuncs.Con_Printf("ChatForwarder: Debugger detected.\n");
        }

        g_winsock = std::unique_ptr<WinsockRAII>(new WinsockRAII());
        if (!g_winsock->IsInitialized()) {
            if (g_pMetaHookAPI) {
                g_pMetaHookAPI->SysError("Failed to initialize Winsock");
            }
            return;
        }

        if (gEngfuncs.pfnRegisterVariable) {
            cf_server_ip = gEngfuncs.pfnRegisterVariable("cf_server_ip", "127.0.0.1", FCVAR_ARCHIVE);
            cf_server_port = gEngfuncs.pfnRegisterVariable("cf_server_port", "26000", FCVAR_ARCHIVE);
            cf_listen_port = gEngfuncs.pfnRegisterVariable("cf_listen_port", "26001", FCVAR_ARCHIVE);
            cf_enabled = gEngfuncs.pfnRegisterVariable("cf_enabled", "1", FCVAR_ARCHIVE);
            cf_debug = gEngfuncs.pfnRegisterVariable("cf_debug", "0", FCVAR_ARCHIVE);
            cf_listen_only = gEngfuncs.pfnRegisterVariable("cf_listen_only", "0", FCVAR_ARCHIVE);
            cf_command_delay = gEngfuncs.pfnRegisterVariable("cf_command_delay", "0", FCVAR_ARCHIVE);
        }

        // Hook OutputDebugStringA in engine to capture everything DebugView sees
        // Only hook once!
        g_pMetaHookAPI->IATHook(g_pMetaHookAPI->GetEngineModule(), "kernel32.dll", "OutputDebugStringA", NewOutputDebugStringA, (void**)&g_pfnOutputDebugStringA);

        bInitialized = true;
    }

    // Ensure thread pool is valid
    g_hThreadPool = g_pMetaHookAPI->GetGlobalThreadPool();
    if (!g_hThreadPool) {
        if (g_pMetaHookAPI) {
            g_pMetaHookAPI->SysError("Failed to get MetaHook thread pool");
        }
        return;
    }

    // Restart Sender Thread if not running
    if (!g_hSenderWorkItem) {
        g_shutdownSender.store(false, std::memory_order_relaxed);
        g_hSenderWorkItem = g_pMetaHookAPI->CreateWorkItem(g_hThreadPool, SenderWorkCallback, nullptr);

        if (!g_hSenderWorkItem) {
            if (g_pMetaHookAPI) {
                g_pMetaHookAPI->SysError("Failed to create sender work item");
            }
            return;
        }
        g_pMetaHookAPI->QueueWorkItem(g_hThreadPool, g_hSenderWorkItem);
    }

    // Restart Listener Thread if not running (and allowed)
    if ((!IsCvarValid(cf_listen_only) || atoi(cf_listen_only->string) == 0) && !g_hListenerWorkItem)
    {
        g_shutdownListener.store(false, std::memory_order_relaxed);
        g_hListenerWorkItem = g_pMetaHookAPI->CreateWorkItem(g_hThreadPool, UDPListenerWorkCallback, nullptr);

        if (!g_hListenerWorkItem) {
            if (g_pMetaHookAPI) {
                g_pMetaHookAPI->SysError("Failed to create listener work item");
            }
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
}
void IPluginsV4::ExitGame(int iResult)
{
    CleanupResources();
}
const char* IPluginsV4::GetVersion(void)
{
    return "1.4.3";
}
EXPOSE_SINGLE_INTERFACE(IPluginsV4, IPluginsV4, METAHOOK_PLUGIN_API_VERSION_V4);