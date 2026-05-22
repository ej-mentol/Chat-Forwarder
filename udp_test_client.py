import socket
import threading
import struct
import datetime
import os
import time
import argparse
import sys

# ==============================================================================
# DEFAULTS
# ==============================================================================
DEFAULT_SERVER_IP = "127.0.0.1"
DEFAULT_SEND_PORT = 26001
DEFAULT_LISTEN_PORT = 26000
DUP_DELTA = 0.2  # 200ms suppression window

# ==============================================================================
# CONSTANTS & MAPPING
# ==============================================================================
TAG_MAP = {
    0x12: "[CHAT] ",
    0x13: "[GAME] ",
    0x14: "[NET]  ",
    0x15: "[SYS]  ",
    0x16: "[STUFF]"
}

CF_HEADER_SIZE = 9  # 1 (tag) + 8 (steamid64 LE)

# ANSI Colors
ANSI_RESET  = "\033[0m"
ANSI_NORMAL = "\033[0m"       # 0x01
ANSI_NAME   = "\033[1;36m"    # 0x02 (Bold Cyan)
ANSI_TEAM   = "\033[33m"      # 0x03 (Yellow)
ANSI_GREEN  = "\033[32m"      # 0x04 (Green)
ANSI_GREY   = "\033[90m"      # Timestamp
ANSI_STEAM  = "\033[38;5;214m" # Orange for SteamID

def enable_ansi_windows():
    """Enable ANSI escape sequences on Windows 10+ conhost"""
    if os.name == 'nt':
        try:
            from ctypes import windll, c_int, byref
            stdout_handle = windll.kernel32.GetStdHandle(c_int(-11))
            mode = c_int(0)
            windll.kernel32.GetConsoleMode(stdout_handle, byref(mode))
            mode.value |= 0x0004
            windll.kernel32.SetConsoleMode(stdout_handle, mode)
        except Exception:
            pass

def parse_goldsrc_colors(data_bytes):
    """Converts GoldSrc color bytes (0x01-0x04) to ANSI escape codes."""
    res_bytes = bytearray()
    mapping = {
        0x01: ANSI_NORMAL.encode('utf-8'),
        0x02: ANSI_NAME.encode('utf-8'),
        0x03: ANSI_TEAM.encode('utf-8'),
        0x04: ANSI_GREEN.encode('utf-8'),
    }
    for byte in data_bytes:
        if byte in mapping:
            res_bytes.extend(mapping[byte])
        elif byte == 0x0A:  # Newline
            res_bytes.extend(b"\n")
        elif byte == 0x0D:  # CR - skip
            pass
        elif byte >= 0x20 or byte > 0x7F:  # Printable or UTF-8 continuation
            res_bytes.append(byte)
    try:
        return res_bytes.decode('utf-8', errors='replace') + ANSI_RESET
    except Exception:
        return res_bytes.decode('latin-1', errors='replace') + ANSI_RESET

def strip_goldsrc_colors(data_bytes):
    """Removes GoldSrc color control bytes for comparison."""
    return bytes([b for b in data_bytes if b > 0x04 and b != 0x0D])

def format_steamid(steamid: int) -> str:
    """Format SteamID64 for display. Returns 'LAN/Bot' if 0, or STEAM_0:Y:Z."""
    if steamid == 0:
        return "LAN/Bot"
    
    # Standard SteamID64 to SteamID2 (Legacy) conversion
    # Formula: STEAM_0:Y:Z
    # Y = (SteamID64 - 76561197960265728) % 2
    # Z = (SteamID64 - 76561197960265728) // 2
    BASE_ID64 = 76561197960265728
    if steamid >= BASE_ID64:
        account_id = steamid - BASE_ID64
        y = account_id % 2
        z = account_id // 2
        return f"STEAM_0:{y}:{z}"
    
    return str(steamid) # Fallback if it's not a standard individual account

class DuplicateFilter:
    def __init__(self, enabled=True, delta=0.2):
        self.enabled = enabled
        self.delta = delta
        self.history = {} # cleaned_payload -> last_seen_time

    def is_duplicate(self, payload):
        if not self.enabled:
            return False
        
        now = time.time()
        clean = strip_goldsrc_colors(payload)
        
        # Prune old entries
        self.history = {c: t for c, t in self.history.items() if now - t < self.delta}
        
        if clean in self.history:
            return True
        
        self.history[clean] = now
        return False

def get_local_ips():
    try:
        # Get all IP addresses associated with the hostname
        hostname = socket.gethostname()
        ips = socket.gethostbyname_ex(hostname)[2]
        # Include 0.0.0.0 and 127.0.0.1 for convenience
        all_ips = ["0.0.0.0", "127.0.0.1"] + sorted(list(set(ips) - {"127.0.0.1"}))
        return all_ips
    except Exception:
        return ["0.0.0.0", "127.0.0.1"]

def select_interface():
    ips = get_local_ips()
    print("\n[INIT] Available network interfaces:")
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

def listener_thread(bind_ip, listen_port, stop_event, dup_filter):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)

    try:
        sock.bind((bind_ip, listen_port))
        sock.settimeout(1.0)
        print(f"[INFO] Listener bound to {bind_ip}:{listen_port}")
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

            if dup_filter.is_duplicate(payload):
                continue

            timestamp  = f"{ANSI_GREY}[{datetime.datetime.now().strftime('%H:%M:%S')}]{ANSI_RESET}"
            tag_label  = TAG_MAP.get(tag_byte, f"[0x{tag_byte:02X}]")
            message    = parse_goldsrc_colors(payload)

            if tag_byte == 0x12: # [CHAT]
                # Only CHAT carries the local player's SteamID
                steam_str  = format_steamid(steamid)
                print(f"{timestamp} {tag_label} ({ANSI_STEAM}{steam_str}{ANSI_RESET}) {message}")
            else:
                # All other types (SYS, GAME, NET, STUFF) always have steamid=0
                print(f"{timestamp} {tag_label} {message}")

        except socket.timeout:
            continue
        except Exception as e:
            if not stop_event.is_set():
                print(f"[ERROR] Receive error: {e}")

    sock.close()
    print("[INFO] Listener stopped.")

def main():
    enable_ansi_windows()

    parser = argparse.ArgumentParser(description="ChatForwarder UDP Test Client")
    parser.add_argument("-b", "--bind", help="IP address to bind the listener to")
    parser.add_argument("-s", "--server", default=DEFAULT_SERVER_IP, help=f"Target server IP (default: {DEFAULT_SERVER_IP})")
    parser.add_argument("-lp", "--lport", type=int, default=DEFAULT_LISTEN_PORT, help=f"Listen port (default: {DEFAULT_LISTEN_PORT})")
    parser.add_argument("-sp", "--sport", type=int, default=DEFAULT_SEND_PORT, help=f"Send port (default: {DEFAULT_SEND_PORT})")
    parser.add_argument("--show-dups", action="store_true", help="Show all messages (disables 200ms suppression)")
    
    args = parser.parse_args()

    bind_ip = args.bind
    if not bind_ip:
        bind_ip = select_interface()

    dup_filter = DuplicateFilter(enabled=not args.show_dups, delta=DUP_DELTA)

    print("\n" + "="*50)
    print(" ChatForwarder UDP Test Client")
    print(f" Target:    {args.server}:{args.sport}")
    print(f" Listening: {bind_ip}:{args.lport}")
    print(f" Filtering: {'OFF (Show all)' if args.show_dups else 'ON (200ms window)'}")
    print(" SteamID:   0 = LAN or Bot")
    print(" Hint:      Use --help to see all options")
    print("="*50)

    stop_event = threading.Event()
    t = threading.Thread(target=listener_thread, args=(bind_ip, args.lport, stop_event, dup_filter), daemon=True)
    t.start()

    # Wait a bit for listener to bind or fail
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
                print(f"[ERROR] Send failed: {e}")

    except (KeyboardInterrupt, EOFError):
        print("\n[INFO] Shutting down...")
    finally:
        stop_event.set()
        try:
            # Short join to avoid blocking KeyboardInterrupt if it's pressed again
            if t.is_alive():
                t.join(timeout=0.5)
        except KeyboardInterrupt:
            pass
        send_sock.close()

if __name__ == "__main__":
    main()
