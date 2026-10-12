#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <utility>

constexpr unsigned char CF_PONG_MARK = 0x1F;

struct NetCfg {
    std::string bindIp = "0.0.0.0";
    std::string serverIp = "127.0.0.1";
    std::string allowIp = "*";
    int serverPort = 26000;
    int listenPort = 26001;
    bool sayOnly = false;
    bool listenOnly = false;
};

struct NetDiag {
    std::mutex mtx;
    NetCfg cfg;
    bool cfgValid = false;
    uint32_t gen = 1;
    std::string listenState = "not started";
    std::string senderState = "not started";
    std::string lastRxFrom = "-";
    std::string lastRxHead = "-";
    std::string serverSeesMe = "-";
    std::deque<std::pair<int, std::string>> logq;
    std::atomic<uint64_t> rx{0}, rxCmd{0}, rxPong{0}, rxDenied{0}, rxFiltered{0}, tx{0}, txErr{0};
    std::atomic<int> lastTxErr{0};
    std::atomic<int> rttMs{-1};
    std::atomic<uint32_t> pingNonce{0};
    std::atomic<int64_t> lastRxMs{0}, lastTxMs{0}, lastPongMs{0}, pingSentMs{0};
};

inline NetDiag& Diag() {
    static NetDiag d;
    return d;
}

inline int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

inline void DiagLog(int level, const char* fmt, ...) {
    char buf[320];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    NetDiag& d = Diag();
    std::lock_guard<std::mutex> lk(d.mtx);
    if (d.logq.size() >= 64) d.logq.pop_front();
    d.logq.emplace_back(level, buf);
}

inline void SetState(std::string NetDiag::*field, const char* who, const std::string& s, bool ok) {
    NetDiag& d = Diag();
    {
        std::lock_guard<std::mutex> lk(d.mtx);
        if (d.*field == s) return;
        d.*field = s;
    }
    DiagLog(ok ? 1 : 0, "%s: %s", who, s.c_str());
}

inline uint32_t CfgGet(NetCfg& out) {
    NetDiag& d = Diag();
    std::lock_guard<std::mutex> lk(d.mtx);
    out = d.cfg;
    return d.cfgValid ? d.gen : 0;
}

inline void CfgSet(const NetCfg& n, bool force) {
    NetDiag& d = Diag();
    std::lock_guard<std::mutex> lk(d.mtx);
    if (force || !d.cfgValid || n.bindIp != d.cfg.bindIp || n.listenPort != d.cfg.listenPort) ++d.gen;
    d.cfg = n;
    d.cfgValid = true;
}

inline void ResetCounters() {
    NetDiag& d = Diag();
    d.rx = 0; d.rxCmd = 0; d.rxPong = 0; d.rxDenied = 0; d.rxFiltered = 0; d.tx = 0; d.txErr = 0;
    d.lastTxErr = 0; d.rttMs = -1; d.lastRxMs = 0; d.lastTxMs = 0; d.lastPongMs = 0;
}

inline bool ParseIp(const std::string& s, in_addr& out) {
    return inet_pton(AF_INET, s.c_str(), &out) == 1;
}

inline bool IsLoopbackIp(const std::string& s) {
    in_addr a;
    return ParseIp(s, a) && (ntohl(a.s_addr) >> 24) == 127;
}

inline std::string AddrStr(const sockaddr_in& a) {
    char ip[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, (const void*)&a.sin_addr, ip, sizeof(ip));
    return std::string(ip) + ":" + std::to_string(ntohs(a.sin_port));
}

inline const char* WsaHint(int e) {
    switch (e) {
    case WSAEADDRINUSE: return "port already in use (other process or stale instance)";
    case WSAEADDRNOTAVAIL: return "address is not on this machine (check cf_bind_ip)";
    case WSAEACCES: return "access denied / exclusive bind conflict";
    case WSAENETUNREACH: return "no route to destination";
    case WSAEHOSTUNREACH: return "host unreachable";
    case WSAENETDOWN: return "network is down";
    case WSAEINVAL: return "invalid argument (bind ip vs destination mismatch?)";
    case WSAEMSGSIZE: return "datagram too large";
    default: return "";
    }
}

inline std::string WsaErr(const char* what, int e) {
    char buf[200];
    const char* hint = WsaHint(e);
    if (hint[0]) {
        snprintf(buf, sizeof(buf), "%s failed, WSA %d (%s)", what, e, hint);
    } else {
        snprintf(buf, sizeof(buf), "%s failed, WSA %d", what, e);
    }
    return buf;
}

inline SOCKET OpenUdp(const std::string& bindIp, int port, bool exclusive, int timeoutMs, std::string& info) {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        info = WsaErr("socket()", WSAGetLastError());
        return INVALID_SOCKET;
    }
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<u_short>(port));
    if (!ParseIp(bindIp, a.sin_addr)) {
        info = "bad cf_bind_ip '" + bindIp + "'";
        closesocket(s);
        return INVALID_SOCKET;
    }
    if (exclusive) {
        BOOL yes = TRUE;
        setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&yes), sizeof(yes));
    }
    if (timeoutMs > 0) {
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));
    }
    if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == SOCKET_ERROR) {
        info = WsaErr(("bind " + bindIp + ":" + std::to_string(port)).c_str(), WSAGetLastError());
        closesocket(s);
        return INVALID_SOCKET;
    }
    sockaddr_in b = {};
    int len = sizeof(b);
    getsockname(s, reinterpret_cast<sockaddr*>(&b), &len);
    info = "bound " + AddrStr(b);
    return s;
}

void NetDiag_Init(void);
void NetDiag_Frame(void);
void NetDiag_Apply(bool force);
void NetDiag_HandlePong(const std::string& msg);
