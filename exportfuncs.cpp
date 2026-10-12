// exportfuncs.cpp
#define _WIN32_WINNT 0x0600
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include "plugins.h"
#include "netdiag.h"
#include "HLSDK/common/parsemsg.h"
#include <string>
#include <cstring>
#include <new>
#include <chrono>
#include <thread>
#include <vector>

#pragma comment(lib, "Ws2_32.lib")

std::string CleanMessage(const char* input) {
    std::string out;
    if (!input) return out;

    while (*input) {
        unsigned char c = (unsigned char)*input;
        // Allow printable characters, color codes (1-4), \r, \n, \t. Skip bell (7).
        if (c >= 0x20 || c <= 0x04 || c == '\r' || c == '\n' || c == '\t') {
            out += *input;
        }
        input++;
    }
    return out;
}

pfnUserMsgHook g_pfnSayText = NULL;
fn_parsefunc g_pfnCL_ParsePrint = NULL;
fn_parsefunc g_pfnCL_ParseStuffText = NULL;

// ---------------------------------------------------------------------------
// QueueTask - single entry point for all 5 message types.
// Fills a SendTask with the message text, current SteamID64, and routing info.
// The binary packet (tag + steamid64 LE + text) is assembled in SenderWorkCallback.
// ---------------------------------------------------------------------------
void QueueTask(uint8_t tag, const std::string& msg, uint64_t steamid) {
    if (msg.empty()) return;

    const size_t copyLen = (msg.size() < CF_MAX_TEXT) ? msg.size() : CF_MAX_TEXT - 1;

    SendTask task;
    task.tag     = tag;
    task.steamid = steamid; // 0 = no Steam / LAN / not applicable
    memcpy(task.text, msg.c_str(), copyLen);
    task.msglen  = static_cast<uint16_t>(copyLen);

    {
        std::lock_guard<std::mutex> lock(g_netConfig.mtx);
        if (!g_netConfig.valid) return; // cvars not registered yet
        strncpy_s(task.server_ip, g_netConfig.server_ip, sizeof(task.server_ip) - 1);
        task.port = g_netConfig.port;
    }

    g_sendQueue.push(task);
}

void __MsgFunc_Print(void) {
    char* psz = READ_STRING();
    if (psz && psz[0]) {
        std::string cleanMsg = CleanMessage(psz);
        if (!cleanMsg.empty()) {
            if (IsCvarValid(cf_debug) && atoi(cf_debug->string) > 0) {
                gEngfuncs.Con_Printf("[CF] [NET] %s", cleanMsg.c_str());
            }
            QueueTask(MSG_TYPE_NET, cleanMsg);
        }
    }
    if (g_pfnCL_ParsePrint) g_pfnCL_ParsePrint();
}

void __MsgFunc_StuffText(void) {
    char* psz = READ_STRING();
    if (psz && psz[0]) {
        std::string cleanMsg = CleanMessage(psz);
        if (!cleanMsg.empty()) {
            if (IsCvarValid(cf_debug) && atoi(cf_debug->string) > 0) {
                gEngfuncs.Con_Printf("[CF] [STUFF] %s", cleanMsg.c_str());
            }
            QueueTask(MSG_TYPE_STUFF, cleanMsg);
        }
    }
    if (g_pfnCL_ParseStuffText) g_pfnCL_ParseStuffText();
}

void HUD_Init(void) {
    ChatForwarder_Init();
    if (g_pfnHUD_Init) {
        g_pfnHUD_Init();
    }

    // HUD_Init is called on every map load / reconnect.
    // HookUserMsg and HookCLParseFuncByName must only be called once:
    // calling them again would cause the returned "previous" pointer to point
    // back to our own hook, creating infinite recursion.
    static bool bHooksInstalled = false;
    if (!bHooksInstalled) {
        g_pfnSayText           = g_pMetaHookAPI->HookUserMsg("SayText", __MsgFunc_SayText);
        g_pfnTextMsg           = g_pMetaHookAPI->HookUserMsg("TextMsg", __MsgFunc_TextMsg);
        g_pfnCL_ParsePrint     = g_pMetaHookAPI->HookCLParseFuncByName("print",      __MsgFunc_Print);
        g_pfnCL_ParseStuffText = g_pMetaHookAPI->HookCLParseFuncByName("stufftext",  __MsgFunc_StuffText);
        bHooksInstalled = true;
    }
}

void HUD_Frame(double time) {
    NetDiag_Frame();
    g_netConfig.enabled.store(IsCvarValid(cf_enabled) && atoi(cf_enabled->string) != 0, std::memory_order_relaxed);

    auto now = std::chrono::steady_clock::now();
    double delay = IsCvarValid(cf_command_delay) ? (double)cf_command_delay->value : 0.0;
    double elapsed = std::chrono::duration<double>(now - g_lastCommandTime).count();

    if (elapsed >= delay) {
        std::string message;
        if (g_messageQueue.pop(message, 0)) {
            if (!message.empty()) {
                while (!message.empty() && (unsigned char)message.back() <= 32) {
                    message.pop_back();
                }

                if (!message.empty()) {
                    if (IsCvarValid(cf_debug) && atoi(cf_debug->string) > 0) {
                        gEngfuncs.Con_Printf("[CF] Executing: %s\n", message.c_str());
                    }
                    message += '\n';
                    gEngfuncs.pfnClientCmd(message.c_str());
                    g_lastCommandTime = now;
                }
            }
        }
    }
    if (g_pfnHUD_Frame) g_pfnHUD_Frame(time);
}

int HUD_GetStudioModelInterface(int version, struct r_studio_interface_s **ppinterface, struct engine_studio_api_s *pstudio) {
    if (pstudio) {
        memcpy(&g_engineStudio, pstudio, sizeof(g_engineStudio));
    }
    if (g_pfnHUD_GetStudioModelInterface) {
        return g_pfnHUD_GetStudioModelInterface(version, ppinterface, pstudio);
    }
    return 0;
}

int __MsgFunc_SayText(const char* pszName, int iSize, void* pbuf) {
    // Null-safe passthrough: HookUserMsg may return NULL if message wasn't registered
    auto callOriginal = [&]() -> int {
        return g_pfnSayText ? g_pfnSayText(pszName, iSize, pbuf) : 1;
    };

    if (!IsCvarValid(cf_enabled) || atoi(cf_enabled->string) == 0) return callOriginal();

    char temp_buf[1024];
    if (!pbuf || iSize <= 0) return callOriginal();
    if (iSize >= sizeof(temp_buf)) return callOriginal();
    memcpy(temp_buf, pbuf, iSize);

    BEGIN_READ(temp_buf, iSize);
    int client_index = READ_BYTE(); // client index

    char* msg_base = READ_STRING();
    std::string fullMsg = msg_base ? CleanMessage(msg_base) : "";

    for (int i = 0; i < 4; i++) {
        char* arg = READ_STRING();
        if (arg && arg[0]) {
            std::string cleanArg = CleanMessage(arg);
            size_t pos = fullMsg.find("%s");
            if (pos != std::string::npos) fullMsg.replace(pos, 2, cleanArg);
            else { if (!fullMsg.empty()) fullMsg += " "; fullMsg += cleanArg; }
        }
    }

    if (!fullMsg.empty()) {
        // Resolve author's SteamID
        uint64_t authorSteamID = 0;
        const char* idSrc = "none";
        static const bool isSven = (g_pMetaHookAPI && g_pMetaHookAPI->GetEngineType() == ENGINE_SVENGINE);
        const int maxClients = (gEngfuncs.GetMaxClients && gEngfuncs.GetMaxClients() > 0) ? gEngfuncs.GetMaxClients() : 32;
        const bool validClient = (client_index > 0 && client_index <= maxClients);

        // 1. Try engine studio interface on Sven Co-op (player_info_sc_t)
        if (isSven && validClient && g_engineStudio.PlayerInfo) {
            auto* sc_info = (player_info_sc_t*)g_engineStudio.PlayerInfo(client_index - 1);
            if (sc_info && sc_info->m_nSteamID != 0) {
                authorSteamID = sc_info->m_nSteamID;
                idSrc = "studio";
            }
        }

        // 2. Fallback to hud_player_info_t (standard GoldSrc)
        if (authorSteamID == 0 && validClient && gEngfuncs.pfnGetPlayerInfo) {
            hud_player_info_t info = {};
            gEngfuncs.pfnGetPlayerInfo(client_index, &info);
            if (info.m_nSteamID != 0) {
                authorSteamID = info.m_nSteamID;
                idSrc = "hud";
            }
        }

        // 3. Fallback to local player SteamID for own messages
        if (authorSteamID == 0) {
            cl_entity_t* localPlayer = gEngfuncs.GetLocalPlayer ? gEngfuncs.GetLocalPlayer() : nullptr;
            if (localPlayer && localPlayer->index == client_index) {
                authorSteamID = GetLocalSteamID64();
                if (authorSteamID != 0) {
                    idSrc = "local";
                }
            }
        }

        if (authorSteamID != 0) {
            CSteamID sid(authorSteamID);
            if (!sid.IsValid() || !sid.BIndividualAccount()) {
                authorSteamID = 0;
            }
        }

        if (IsCvarValid(cf_debug) && atoi(cf_debug->string) > 0) {
            gEngfuncs.Con_Printf("[CF] [CHAT] (client %d, steamid %llu, src %s) %s\n",
                client_index, (unsigned long long)authorSteamID, idSrc, fullMsg.c_str());
        }
        QueueTask(MSG_TYPE_CHAT, fullMsg, authorSteamID);
    }

    return callOriginal();
}

int __MsgFunc_TextMsg(const char* pszName, int iSize, void* pbuf) {
    // Null-safe passthrough: HookUserMsg may return NULL if message wasn't registered
    auto callOriginal = [&]() -> int {
        return g_pfnTextMsg ? g_pfnTextMsg(pszName, iSize, pbuf) : 1;
    };

    char temp_buf[1024];
    if (!pbuf || iSize <= 0) return callOriginal();
    if (iSize >= sizeof(temp_buf)) return callOriginal();
    memcpy(temp_buf, pbuf, iSize);

    BEGIN_READ(temp_buf, iSize);
    int msg_dest = READ_BYTE();
    char* msg_text = READ_STRING();

    if (msg_dest >= 1 && msg_dest <= 4 && IsCvarValid(cf_enabled) && atoi(cf_enabled->string) == 1) {
        std::string fullMsg = msg_text ? CleanMessage(msg_text) : "";
        for (int i = 0; i < 4; i++) {
            char* arg = READ_STRING();
            if (arg && arg[0]) {
                std::string cleanArg = CleanMessage(arg);
                size_t pos = fullMsg.find("%s");
                if (pos != std::string::npos) fullMsg.replace(pos, 2, cleanArg);
                else { if (!fullMsg.empty()) fullMsg += " "; fullMsg += cleanArg; }
            }
        }
        if (!fullMsg.empty()) {
            if (IsCvarValid(cf_debug) && atoi(cf_debug->string) > 0) {
                gEngfuncs.Con_Printf("[CF] [GAME] %s\n", fullMsg.c_str());
            }
            QueueTask(MSG_TYPE_GAME, fullMsg);
        }
    }
    return callOriginal();
}
