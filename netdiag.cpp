#include "plugins.h"
#include "netdiag.h"
#include <cstdlib>
#include <cstring>

static cvar_t* cf_bind_ip = NULL;
static cvar_t* cf_say_only = NULL;

static bool CvarOn(cvar_t* c) {
    return IsCvarValid(c) && atoi(c->string) != 0;
}

static std::string Age(int64_t ms) {
    if (ms == 0) return "never";
    char b[32];
    snprintf(b, sizeof(b), "%.1fs ago", (NowMs() - ms) / 1000.0);
    return b;
}

static void SendPing(void) {
    NetDiag& d = Diag();
    uint32_t n = ++d.pingNonce;
    d.pingSentMs = NowMs();
    QueueTask(MSG_TYPE_PING, "ping " + std::to_string(n));
}

static std::string RouteProbe(const std::string& ip, int port) {
    in_addr a;
    if (!ParseIp(ip, a)) return "bad cf_server_ip";
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return WsaErr("socket()", WSAGetLastError());
    sockaddr_in to = {};
    to.sin_family = AF_INET;
    to.sin_addr = a;
    to.sin_port = htons(static_cast<u_short>(port));
    std::string out;
    if (connect(s, reinterpret_cast<sockaddr*>(&to), sizeof(to)) == SOCKET_ERROR) {
        out = WsaErr("route", WSAGetLastError());
    } else {
        sockaddr_in me = {};
        int len = sizeof(me);
        getsockname(s, reinterpret_cast<sockaddr*>(&me), &len);
        char ipbuf[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &me.sin_addr, ipbuf, sizeof(ipbuf));
        out = ipbuf;
    }
    closesocket(s);
    return out;
}

static void CF_Status_f(void) {
    NetDiag& d = Diag();
    NetCfg c;
    uint32_t gen = CfgGet(c);
    std::string ls, ss, from, head, seen;
    {
        std::lock_guard<std::mutex> lk(d.mtx);
        ls = d.listenState;
        ss = d.senderState;
        from = d.lastRxFrom;
        head = d.lastRxHead;
        seen = d.serverSeesMe;
    }
    gEngfuncs.Con_Printf("ChatForwarder Network Diagnostics:\n");
    gEngfuncs.Con_Printf("  Status   : enabled=%d, listen_only=%d, say_only=%d, cfg_gen=%u\n",
        CvarOn(cf_enabled), CvarOn(cf_listen_only), c.sayOnly, gen);
    gEngfuncs.Con_Printf("  Listener : %s\n", ls.c_str());
    gEngfuncs.Con_Printf("  Sender   : %s -> %s:%d\n", ss.c_str(), c.serverIp.c_str(), c.serverPort);
    std::string src = RouteProbe(c.serverIp, c.serverPort);
    gEngfuncs.Con_Printf("  Route    : OS uses local %s to reach %s\n", src.c_str(), c.serverIp.c_str());
    gEngfuncs.Con_Printf("  Allow IP : %s (denied %llu)\n", c.allowIp.c_str(), (unsigned long long)d.rxDenied.load());
    if (head != "-" && !head.empty()) {
        gEngfuncs.Con_Printf("  RX       : %llu total, %llu cmds, %llu filtered, last %s from %s (\"%s\")\n",
            (unsigned long long)d.rx.load(), (unsigned long long)d.rxCmd.load(),
            (unsigned long long)d.rxFiltered.load(), Age(d.lastRxMs).c_str(), from.c_str(), head.c_str());
    } else {
        gEngfuncs.Con_Printf("  RX       : %llu total, %llu cmds, %llu filtered, last %s from %s\n",
            (unsigned long long)d.rx.load(), (unsigned long long)d.rxCmd.load(),
            (unsigned long long)d.rxFiltered.load(), Age(d.lastRxMs).c_str(), from.c_str());
    }
    gEngfuncs.Con_Printf("  TX       : %llu ok, %llu err (last WSA %d), last %s\n",
        (unsigned long long)d.tx.load(), (unsigned long long)d.txErr.load(), d.lastTxErr.load(), Age(d.lastTxMs).c_str());
    gEngfuncs.Con_Printf("  Pong     : %llu, rtt %d ms, last %s, server sees me as %s\n",
        (unsigned long long)d.rxPong.load(), d.rttMs.load(), Age(d.lastPongMs).c_str(), seen.c_str());

    if (IsLoopbackIp(c.bindIp) && !IsLoopbackIp(c.serverIp))
        gEngfuncs.Con_Printf("  WARNING  : bind_ip is loopback but server_ip is not\n");
    if (!c.allowIp.empty() && c.allowIp != "*")
        gEngfuncs.Con_Printf("  WARNING  : allow_ip filters by packet source; a peer sending to a LAN IP of this PC arrives from that LAN IP, not 127.0.0.1\n");
    if (d.pingNonce > 0 && d.rxPong == 0)
        gEngfuncs.Con_Printf("  WARNING  : pings sent, no pong: server not replying, wrong IP, or firewall\n");
    if (ls.compare(0, 5, "bound") != 0)
        gEngfuncs.Con_Printf("  WARNING  : listener is NOT bound\n");
}

static void CF_Ping_f(void) {
    SendPing();
    gEngfuncs.Con_Printf("[CF] Ping queued (check cf_status in a second).\n");
}

static void CF_Reload_f(void) {
    NetDiag_Apply(true);
    gEngfuncs.Con_Printf("[CF] Reloaded: cvars re-read, sockets rebinding, counters reset.\n");
}

static void CF_Ifaces_f(void) {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return;
    INTERFACE_INFO list[32];
    DWORD got = 0;
    if (WSAIoctl(s, SIO_GET_INTERFACE_LIST, NULL, 0, list, sizeof(list), &got, NULL, NULL) == SOCKET_ERROR) {
        gEngfuncs.Con_Printf("[CF] %s\n", WsaErr("interface list", WSAGetLastError()).c_str());
        closesocket(s);
        return;
    }
    closesocket(s);
    NetCfg c;
    CfgGet(c);
    std::string used = RouteProbe(c.serverIp, c.serverPort);
    int n = static_cast<int>(got / sizeof(INTERFACE_INFO));
    gEngfuncs.Con_Printf("ChatForwarder Network Interfaces:\n");
    for (int i = 0; i < n; ++i) {
        const sockaddr_in& a = list[i].iiAddress.AddressIn;
        char ip[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &a.sin_addr, ip, sizeof(ip));
        gEngfuncs.Con_Printf("  %-15s %s%s%s\n", ip,
            (list[i].iiFlags & IFF_UP) ? "up" : "down",
            (list[i].iiFlags & IFF_LOOPBACK) ? " loopback" : "",
            used == ip ? " <- OS route to cf_server_ip" : "");
    }
}

void NetDiag_Init(void) {
    static bool done = false;
    if (done) return;
    done = true;
    if (gEngfuncs.pfnRegisterVariable) {
        cf_bind_ip = gEngfuncs.pfnRegisterVariable("cf_bind_ip", "0.0.0.0", FCVAR_ARCHIVE);
        cf_say_only = gEngfuncs.pfnRegisterVariable("cf_say_only", "0", FCVAR_ARCHIVE);
    }
    if (gEngfuncs.pfnAddCommand) {
        gEngfuncs.pfnAddCommand("cf_status", CF_Status_f);
        gEngfuncs.pfnAddCommand("cf_ping", CF_Ping_f);
        gEngfuncs.pfnAddCommand("cf_ifaces", CF_Ifaces_f);
        gEngfuncs.pfnAddCommand("cf_reload", CF_Reload_f);
    }
    NetDiag_Apply(false);
}

void NetDiag_Apply(bool force) {
    NetCfg c;
    if (IsCvarValid(cf_server_ip)) c.serverIp = cf_server_ip->string;
    if (IsCvarValid(cf_server_port)) c.serverPort = atoi(cf_server_port->string);
    if (IsCvarValid(cf_allow_ip)) c.allowIp = cf_allow_ip->string;
    if (IsCvarValid(cf_listen_port)) c.listenPort = atoi(cf_listen_port->string);
    if (IsCvarValid(cf_bind_ip)) c.bindIp = cf_bind_ip->string;
    c.sayOnly = CvarOn(cf_say_only);
    c.listenOnly = CvarOn(cf_listen_only);
    CfgSet(c, force);
    if (force) ResetCounters();

    if (IsCvarValid(cf_server_ip) && IsCvarValid(cf_server_port)) {
        std::lock_guard<std::mutex> lock(g_netConfig.mtx);
        snprintf(g_netConfig.server_ip, sizeof(g_netConfig.server_ip), "%s", cf_server_ip->string);
        g_netConfig.port = c.serverPort;
        g_netConfig.valid = true;
    }
}

void NetDiag_Frame(void) {
    static int64_t next = 0;
    int64_t now = NowMs();
    if (now < next) return;
    next = now + 1000;

    NetDiag_Apply(false);

    NetDiag& d = Diag();
    std::deque<std::pair<int, std::string>> out;
    {
        std::lock_guard<std::mutex> lk(d.mtx);
        out.swap(d.logq);
    }
    bool dbg = IsCvarValid(cf_debug) && atoi(cf_debug->string) > 0;
    for (auto& e : out) {
        if (e.first == 0 || dbg) gEngfuncs.Con_Printf("[CF] %s\n", e.second.c_str());
    }
}

void NetDiag_HandlePong(const std::string& msg) {
    const char* p = msg.c_str() + 1;
    if (strncmp(p, "PONG ", 5) != 0) return;
    p += 5;
    char* end = NULL;
    unsigned long nonce = strtoul(p, &end, 10);
    while (end && *end == ' ') ++end;
    NetDiag& d = Diag();
    ++d.rxPong;
    int64_t now = NowMs();
    d.lastPongMs = now;
    if (nonce == d.pingNonce.load()) d.rttMs = static_cast<int>(now - d.pingSentMs.load());

    std::string peerAddr = end ? end : "-";
    while (!peerAddr.empty() && (unsigned char)peerAddr.back() <= 32) {
        peerAddr.pop_back();
    }
    if (peerAddr.empty()) peerAddr = "-";

    std::lock_guard<std::mutex> lk(d.mtx);
    d.serverSeesMe = std::move(peerAddr);
}
