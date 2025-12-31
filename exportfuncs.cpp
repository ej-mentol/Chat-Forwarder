// exportfuncs.cpp
#define _WIN32_WINNT 0x0600
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include "plugins.h"
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
        if (c >= 0x20 || (c >= 0x01 && c <= 0x04) || c == '\r' || c == '\n' || c == '\t') {
            out += *input;
        }
        input++;
    }
    return out;
}

pfnUserMsgHook g_pfnSayText = NULL;
fn_parsefunc g_pfnCL_ParsePrint = NULL;
fn_parsefunc g_pfnCL_ParseStuffText = NULL;

void QueueTask(char tag, const std::string& msg) {
    if (msg.empty()) return;

    std::string fullMsg;
    fullMsg += tag;
    fullMsg += msg;
    
    SendTask task;
    strncpy_s(task.message, fullMsg.c_str(), sizeof(task.message) - 1);
    strncpy_s(task.server_ip, cf_server_ip->string, sizeof(task.server_ip) - 1);
    task.port = atoi(cf_server_port->string);

    g_sendQueue.push(task);
}

void __MsgFunc_Print(void) {
    char* psz = READ_STRING();
    if (psz && psz[0]) {
        std::string cleanMsg = CleanMessage(psz);
        if (!cleanMsg.empty()) {
            if (IsCvarValid(cf_debug) && atoi(cf_debug->string) > 0) {
                gEngfuncs.Con_Printf("[ChatForwarder][NET] %s", cleanMsg.c_str());
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
                gEngfuncs.Con_Printf("[ChatForwarder][STUFF] %s", cleanMsg.c_str());
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

    g_pfnSayText = g_pMetaHookAPI->HookUserMsg("SayText", __MsgFunc_SayText);
    g_pfnTextMsg = g_pMetaHookAPI->HookUserMsg("TextMsg", __MsgFunc_TextMsg);

    g_pfnCL_ParsePrint = g_pMetaHookAPI->HookCLParseFuncByName("print", __MsgFunc_Print);
    g_pfnCL_ParseStuffText = g_pMetaHookAPI->HookCLParseFuncByName("stufftext", __MsgFunc_StuffText);
}

void HUD_Frame(double time) {
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
                        gEngfuncs.Con_Printf("[ChatForwarder] Executing: %s\n", message.c_str());
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

int __MsgFunc_SayText(const char* pszName, int iSize, void* pbuf) {
    if (!IsCvarValid(cf_enabled) || atoi(cf_enabled->string) == 0) return g_pfnSayText(pszName, iSize, pbuf);

    char temp_buf[1024];
    if (iSize >= sizeof(temp_buf)) return g_pfnSayText(pszName, iSize, pbuf);
    memcpy(temp_buf, pbuf, iSize);

    BEGIN_READ(temp_buf, iSize);
    READ_BYTE(); // client index
    
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
        if (IsCvarValid(cf_debug) && atoi(cf_debug->string) > 0) {
            gEngfuncs.Con_Printf("[ChatForwarder][CHAT] %s\n", fullMsg.c_str());
        }
        QueueTask(MSG_TYPE_CHAT, fullMsg);
    }

    return g_pfnSayText(pszName, iSize, pbuf);
}

int __MsgFunc_TextMsg(const char* pszName, int iSize, void* pbuf) {
    char temp_buf[1024];
    if (iSize >= sizeof(temp_buf)) return g_pfnTextMsg(pszName, iSize, pbuf);
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
                gEngfuncs.Con_Printf("[ChatForwarder][GAME] %s\n", fullMsg.c_str());
            }
            QueueTask(MSG_TYPE_GAME, fullMsg);
        }
    }
    return g_pfnTextMsg(pszName, iSize, pbuf);
}
