#!/usr/bin/env python3
"""
ChatForwarder Reference UDP Test Client
=======================================
A standalone test client, connection debugger, and bit-exact protocol reference
for the ChatForwarder GoldSrc / MetaHookSv plugin.

Wire Protocol Specification:
----------------------------
Packets sent by the plugin to the UDP client have the following binary layout:

  Offset  | Type          | Description
  --------+---------------+---------------------------------------------------
  0       | uint8         | Packet Type Tag:
          |               |   0x10 = PING   (Keepalive probe from cf_ping)
          |               |   0x12 = CHAT   (Player chat via SayText)
          |               |   0x13 = GAME   (Game notices via TextMsg)
          |               |   0x14 = NET    (Server console print)
          |               |   0x15 = SYS    (Engine OutputDebugStringA log)
          |               |   0x16 = STUFF  (Server stufftext command)
  1..8    | uint64 (LE)   | SteamID64 of message author (little-endian unsigned).
          |               |   - Valid 64-bit ID for 0x12 (CHAT)
          |               |   - 0 for bots, LAN, or non-chat packets
  9..end  | bytes (UTF-8) | Message payload (may include GoldSrc color bytes 0x01-0x04)

Inbound Commands (Client -> Plugin):
  Send raw UTF-8 string to plugin's cf_listen_port (default 26001).
  If cf_say_only=1 on the server, commands must start with 'say ' or 'say_team '.

PING / PONG Handshake:
  When the plugin issues 'cf_ping', it sends tag 0x10 with payload "ping <nonce>".
  The client replies with byte 0x1F followed by ASCII "PONG <nonce> <client_ip>:<client_port>".
  This allows the plugin (cf_status) to calculate round-trip time (RTT) and discover NAT/peer address.
"""

import socket
import threading
import struct
import datetime
import os
import time
import argparse
import sys

# ==============================================================================
# PROTOCOL CONSTANTS
# ==============================================================================
CF_HEADER_SIZE = 9  # 1 byte (tag) + 8 bytes (steamid64 LE)

# Message type tags sent by plugin
TAG_PING  = 0x10
TAG_CHAT  = 0x12
TAG_GAME  = 0x13
TAG_NET   = 0x14
TAG_SYS   = 0x15
TAG_STUFF = 0x16

# Response tag sent back to plugin for ping keepalives
RESP_TAG_PONG = 0x1F

# Default network configuration
DEFAULT_SERVER_IP   = "127.0.0.1"
DEFAULT_SEND_PORT   = 26001  # Plugin listens for commands here (cf_listen_port)
DEFAULT_LISTEN_PORT = 26000  # Plugin sends UDP messages here (cf_server_port)
DUP_DELTA           = 0.2    # 200ms duplicate suppression window

# ==============================================================================
# ANSI TERMINAL COLORS
# ==============================================================================
ANSI_RESET    = "\033[0m"
ANSI_GREY     = "\033[90m"
ANSI_ORANGE   = "\033[38;5;214m"

# Tag badge colors
TAG_BADGES = {
    TAG_PING:  "\033[1;37m[PING]\033[0m",
    TAG_CHAT:  "\033[1;32m[CHAT]\033[0m",
    TAG_GAME:  "\033[1;33m[GAME]\033[0m",
    TAG_NET:   "\033[36m[NET]\033[0m ",
    TAG_SYS:   "\033[35m[SYS]\033[0m ",
    TAG_STUFF: "\033[31m[STUFF]\033[0m",
}

# GoldSrc in-text color code mapping (0x01 - 0x04)
GOLDSRC_COLORS = {
    0x01: b"\033[0m",       # Normal / default text
    0x02: b"\033[1;36m",    # Player name (Bold Cyan)
    0x03: b"\033[33m",      # Team color (Yellow)
    0x04: b"\033[32m",      # Location / green
}


def enable_ansi_windows():
    """Enable ANSI virtual terminal processing on Windows 10+ conhost."""
    if os.name == 'nt':
        try:
            from ctypes import windll, c_int, byref
            stdout_handle = windll.kernel32.GetStdHandle(c_int(-11))
            mode = c_int(0)
            windll.kernel32.GetConsoleMode(stdout_handle, byref(mode))
            mode.value |= 0x0004  # ENABLE_VIRTUAL_TERMINAL_PROCESSING
            windll.kernel32.SetConsoleMode(stdout_handle, mode)
        except Exception:
            pass


def parse_goldsrc_colors(data_bytes: bytes) -> str:
    """Translate GoldSrc control bytes (0x01-0x04) into ANSI terminal color sequences."""
    res_bytes = bytearray()
    for byte in data_bytes:
        if byte in GOLDSRC_COLORS:
            res_bytes.extend(GOLDSRC_COLORS[byte])
        elif byte == 0x0A:  # Newline
            res_bytes.extend(b"\n")
        elif byte == 0x0D:  # Carriage return
            pass
        elif byte >= 0x20:  # Printable character
            res_bytes.append(byte)
    try:
        return res_bytes.decode('utf-8', errors='replace') + ANSI_RESET
    except Exception:
        return res_bytes.decode('latin-1', errors='replace') + ANSI_RESET


def strip_goldsrc_colors(data_bytes: bytes) -> bytes:
    """Strip color bytes for content deduplication."""
    return bytes([b for b in data_bytes if b > 0x04 and b != 0x0D])


def format_steamid(steamid: int) -> str:
    """Format SteamID64 for display as legacy STEAM_0:Y:Z, or 'LAN/Bot' if 0."""
    if steamid == 0:
        return "LAN/Bot"

    # Standard SteamID64 to SteamID2 conversion
    base_id64 = 76561197960265728
    if steamid >= base_id64:
        account_id = steamid - base_id64
        y = account_id % 2
        z = account_id // 2
        return f"STEAM_0:{y}:{z}"

    return str(steamid)


class DuplicateFilter:
    """Suppresses duplicate consecutive packets arriving within a short delta."""
    def __init__(self, enabled=True, delta=0.2):
        self.enabled = enabled
        self.delta = delta
        self.history = {}
        self._counter = 0

    def is_duplicate(self, tag_byte: int, payload: bytes) -> bool:
        if not self.enabled:
            return False

        now = time.time()
        clean_key = (tag_byte, strip_goldsrc_colors(payload))

        self._counter += 1
        if self._counter % 50 == 0:
            self.history = {k: t for k, t in self.history.items() if now - t < self.delta}

        last_time = self.history.get(clean_key)
        if last_time is not None and now - last_time < self.delta:
            return True

        self.history[clean_key] = now
        return False


def get_local_ips():
    """Enumerate local IP addresses available on the system."""
    try:
        hostname = socket.gethostname()
        ips = socket.gethostbyname_ex(hostname)[2]
        return ["0.0.0.0", "127.0.0.1"] + sorted(list(set(ips) - {"127.0.0.1"}))
    except Exception:
        return ["0.0.0.0", "127.0.0.1"]


def select_interface() -> str:
    """Prompt user to choose a network interface for binding."""
    ips = get_local_ips()
    print("\nAvailable network interfaces:")
    for i, ip in enumerate(ips):
        print(f"  [{i}] {ip}")

    while True:
        try:
            choice = input(f"Select interface (0-{len(ips)-1}, default 0): ").strip()
            if not choice:
                return ips[0]
            idx = int(choice)
            if 0 <= idx < len(ips):
                return ips[idx]
        except (EOFError, KeyboardInterrupt):
            print()
            return ips[0]
        except ValueError:
            pass
        print(f"Invalid selection. Please choose 0-{len(ips)-1}.")


def listener_thread(bind_ip: str, listen_port: int, stop_event: threading.Event,
                    dup_filter: DuplicateFilter, target_server: str, target_port: int):
    """Background thread receiving and rendering UDP packets from the plugin."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)

    try:
        sock.bind((bind_ip, listen_port))
        sock.settimeout(1.0)
    except Exception as e:
        print(f"[ERROR] Failed to bind listener to {bind_ip}:{listen_port}: {e}")
        stop_event.set()
        return

    while not stop_event.is_set():
        try:
            data, addr = sock.recvfrom(4096)
            if len(data) < CF_HEADER_SIZE:
                continue

            tag_byte = data[0]
            steamid  = struct.unpack_from('<Q', data, 1)[0]
            payload  = data[CF_HEADER_SIZE:]

            timestamp = f"{ANSI_GREY}[{datetime.datetime.now().strftime('%H:%M:%S')}]{ANSI_RESET}"
            badge     = TAG_BADGES.get(tag_byte, f"[0x{tag_byte:02X}]")

            # Handle PING keepalive probe
            if tag_byte == TAG_PING:
                try:
                    text_str = payload.decode('utf-8', errors='replace').strip()
                    parts = text_str.split()
                    nonce = parts[1] if len(parts) > 1 else "0"

                    if target_server and target_port:
                        # Wire response: 0x1F + "PONG <nonce> <src_ip>:<src_port>"
                        pong_packet = bytes([RESP_TAG_PONG]) + f"PONG {nonce} {addr[0]}:{addr[1]}".encode('ascii')
                        sock.sendto(pong_packet, (target_server, target_port))
                        print(f"{timestamp} {badge} Nonce {nonce} from {addr[0]}:{addr[1]} -> replied PONG to {target_server}:{target_port}")
                    else:
                        print(f"{timestamp} {badge} Nonce {nonce} from {addr[0]}:{addr[1]}")
                except Exception as e:
                    print(f"[ERROR] Failed to process ping: {e}")
                continue

            # Duplicate suppression check
            if dup_filter.is_duplicate(tag_byte, payload):
                continue

            # Parse GoldSrc color sequences into terminal ANSI colors
            message = parse_goldsrc_colors(payload)

            if tag_byte == TAG_CHAT:
                # CHAT packets carry the author's SteamID
                steam_str = format_steamid(steamid)
                print(f"{timestamp} {badge} ({ANSI_ORANGE}{steam_str}{ANSI_RESET}) {message}")
            else:
                # All other packet types (SYS, GAME, NET, STUFF)
                print(f"{timestamp} {badge} {message}")

        except socket.timeout:
            continue
        except Exception as e:
            if not stop_event.is_set():
                print(f"[ERROR] Receive error: {e}")

    sock.close()


def main():
    enable_ansi_windows()

    parser = argparse.ArgumentParser(
        description="ChatForwarder UDP test client and protocol debugger",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""\
examples:
  python udp_test_client.py
      Local testing (binds to 0.0.0.0:26000, target 127.0.0.1:26001)

  python udp_test_client.py -b 192.168.1.10 -s 192.168.1.20
      LAN testing (listen on local LAN IP, send commands to remote game)
"""
    )
    parser.add_argument("-b", "--bind", help="Local IP to bind the listener socket to (default: interactive prompt)")
    parser.add_argument("-s", "--server", default=DEFAULT_SERVER_IP, help=f"Game server/client IP (default: {DEFAULT_SERVER_IP})")
    parser.add_argument("-lp", "--lport", type=int, default=DEFAULT_LISTEN_PORT, help=f"Local listen port (default: {DEFAULT_LISTEN_PORT})")
    parser.add_argument("-sp", "--sport", type=int, default=DEFAULT_SEND_PORT, help=f"Plugin command port (default: {DEFAULT_SEND_PORT})")
    parser.add_argument("--show-dups", action="store_true", help="Disable 200ms duplicate message suppression")

    args = parser.parse_args()

    bind_ip = args.bind
    if not bind_ip:
        bind_ip = select_interface()

    dup_filter = DuplicateFilter(enabled=not args.show_dups, delta=DUP_DELTA)

    print("\n" + "=" * 60)
    print(" ChatForwarder UDP Test Client")
    print(f" Listening: {bind_ip}:{args.lport} (plugin sends here)")
    print(f" Target:    {args.server}:{args.sport} (commands sent here)")
    print(f" Filtering: {'OFF' if args.show_dups else 'ON (200ms suppression)'}")
    print(" Hint:      Type a console command and press Enter to send.")
    print("            Not seeing messages? Check -b / -s or run 'cf_status' in game.")
    print("=" * 60 + "\n")

    stop_event = threading.Event()
    t = threading.Thread(
        target=listener_thread,
        args=(bind_ip, args.lport, stop_event, dup_filter, args.server, args.sport),
        daemon=True
    )
    t.start()

    time.sleep(0.1)
    if stop_event.is_set():
        sys.exit(1)

    send_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    try:
        while True:
            cmd = input()
            if cmd.lower() in ['quit', 'exit']:
                break
            if not cmd.strip():
                continue
            try:
                send_sock.sendto(cmd.encode('utf-8'), (args.server, args.sport))
            except Exception as e:
                print(f"[ERROR] Failed to send command: {e}")

    except (KeyboardInterrupt, EOFError):
        print("\nShutting down...")
    finally:
        stop_event.set()
        try:
            if t.is_alive():
                t.join(timeout=0.5)
        except KeyboardInterrupt:
            pass
        send_sock.close()


if __name__ == "__main__":
    main()
