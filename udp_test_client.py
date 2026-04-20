import socket
import threading
import sys
import time
import datetime
import os
from collections import deque

# ==============================================================================
# CONFIGURATION
# ==============================================================================
SERVER_IP = "127.0.0.1"
# Port to SEND commands TO (Must match 'cf_listen_port' in game)
SEND_PORT = 26001
# Port to LISTEN for messages FROM (Must match 'cf_server_port' in game)
LISTEN_PORT = 26000

# Filter to specific tag types (empty = show all)
SHOW_TYPES = []

# Deduplication: suppress ANY messages with identical text content arriving
# within an extremely short window. GoldSrc engine loopbacks on Listen Servers
# fire the same text across multiple hooks (e.g. SYS, CHAT, SYS again) in the
# same physical frame (0.00s delay).
# Set DEDUP_WINDOW to 0 to disable.
DEDUP_ENABLED = True
DEDUP_WINDOW  = 0.05   # 50ms window

# ==============================================================================
# CONSTANTS & MAPPING
# ==============================================================================
TAG_MAP = {
    0x12: "[CHAT]",
    0x13: "[GAME]",
    0x14: "[NET] ",
    0x15: "[SYS] ",
    0x16: "[STUFF]",
}

# ANSI Colors
ANSI_RESET  = "\033[0m"
ANSI_NORMAL = "\033[0m"       # 0x01
ANSI_NAME   = "\033[1;36m"    # 0x02 (Bold Cyan)
ANSI_TEAM   = "\033[33m"      # 0x03 (Yellow)
ANSI_GREEN  = "\033[32m"      # 0x04 (Green)
ANSI_GREY   = "\033[90m"      # Timestamp / dim
ANSI_DIM    = "\033[2m"       # Duplicate marker

# ==============================================================================
# DEDUPLICATION
# ==============================================================================
# Each entry: (timestamp_float, content_key_bytes)
_dedup_buf: deque = deque()
_dedup_lock = threading.Lock()

def _content_key(payload: bytes) -> bytes:
    """
    Derive a normalised comparison key from a raw payload:
      - strip GoldSrc color bytes (0x01-0x04)
      - strip leading/trailing whitespace and control chars
      - lowercase
    This allows [SYS] 'mentol: hi' and [CHAT] '\x02mentol\x01: hi'
    to resolve to the same key.
    """
    stripped = bytes(b for b in payload if b >= 0x20)
    return stripped.strip().lower()

def is_duplicate(payload: bytes) -> bool:
    """
    Returns True if the exact same content was seen within DEDUP_WINDOW (e.g. 50ms).
    This cleanly drops any Listen Server loopbacks (SYS + CHAT + SYS)
    while keeping legitimate human spam (which physicaly takes >150ms).
    """
    if not DEDUP_ENABLED or DEDUP_WINDOW <= 0:
        return False

    now = time.monotonic()
    key = _content_key(payload)

    with _dedup_lock:
        # Evict expired entries
        while _dedup_buf and now - _dedup_buf[0][0] > DEDUP_WINDOW:
            _dedup_buf.popleft()

        # Check for exact match
        for ts, k in _dedup_buf:
            if k == key:
                return True

        # Not a duplicate – register it
        _dedup_buf.append((now, key))
        return False

# ==============================================================================
# ANSI / DISPLAY
# ==============================================================================
def enable_ansi_windows():
    """Enable ANSI escape sequences on Windows 10+ conhost."""
    if os.name == 'nt':
        from ctypes import windll, c_int, byref
        stdout_handle = windll.kernel32.GetStdHandle(c_int(-11))
        mode = c_int(0)
        windll.kernel32.GetConsoleMode(stdout_handle, byref(mode))
        mode.value |= 0x0004
        windll.kernel32.SetConsoleMode(stdout_handle, mode)

def parse_goldsrc_colors(data_bytes: bytes) -> str:
    """
    Converts GoldSrc color bytes (0x01-0x04) to ANSI escape codes
    while preserving the rest of the UTF-8 string.
    """
    mapping = {
        0x01: ANSI_NORMAL.encode(),
        0x02: ANSI_NAME.encode(),
        0x03: ANSI_TEAM.encode(),
        0x04: ANSI_GREEN.encode(),
    }
    res = bytearray()
    for byte in data_bytes:
        if byte in mapping:
            res.extend(mapping[byte])
        elif byte == 0x0A:  # LF
            res.extend(b"\n")
        elif byte == 0x0D:  # CR – skip
            pass
        elif byte >= 0x20 or byte > 0x7F:
            res.append(byte)
    try:
        return res.decode('utf-8', errors='replace') + ANSI_RESET
    except Exception:
        return res.decode('latin-1', errors='replace') + ANSI_RESET

# ==============================================================================
# LISTENER THREAD
# ==============================================================================
def listener_thread(stop_event: threading.Event):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)

    try:
        sock.bind(("0.0.0.0", LISTEN_PORT))
        sock.settimeout(1.0)
        print(f"[INFO] Listening on 0.0.0.0:{LISTEN_PORT}")
    except Exception as e:
        print(f"[ERROR] Failed to bind listener: {e}")
        return

    while not stop_event.is_set():
        try:
            data, addr = sock.recvfrom(4096)
            if not data:
                continue

            tag_byte = data[0]
            payload  = data[1:]

            # Tag filter
            if SHOW_TYPES and tag_byte not in SHOW_TYPES:
                continue

            # Deduplication: block if the exact stripped text was shown
            # in the last DEDUP_WINDOW seconds.
            if is_duplicate(payload):
                continue

            timestamp     = f"{ANSI_GREY}[{datetime.datetime.now().strftime('%H:%M:%S')}]{ANSI_RESET}"
            tag_label     = TAG_MAP.get(tag_byte, f"[UNK:0x{tag_byte:02X}]")
            formatted_msg = parse_goldsrc_colors(payload)

            print(f"{timestamp} {tag_label} {formatted_msg}")

        except socket.timeout:
            continue
        except Exception as e:
            print(f"[ERROR] Receive error: {e}")

    sock.close()
    print("[INFO] Listener stopped.")

# ==============================================================================
# MAIN
# ==============================================================================
def main():
    enable_ansi_windows()

    dedup_status = f"on ({int(DEDUP_WINDOW * 1000)}ms window)" if DEDUP_ENABLED else "off"
    print("=================================================")
    print(" ChatForwarder UDP Test Client")
    print(f" Receiving: 0.0.0.0:{LISTEN_PORT}")
    print(f" Sending:   {SERVER_IP}:{SEND_PORT}")
    print(f" Dedup:     {dedup_status}")
    print("=================================================")

    stop_event = threading.Event()
    t = threading.Thread(target=listener_thread, args=(stop_event,), daemon=True)
    t.start()

    send_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    try:
        while True:
            cmd = input()
            if cmd.lower() in ('quit', 'exit'):
                break
            if not cmd.strip():
                continue
            try:
                send_sock.sendto(cmd.encode('utf-8'), (SERVER_IP, SEND_PORT))
            except Exception as e:
                print(f"[ERROR] Send failed: {e}")

    except KeyboardInterrupt:
        print("\n[INFO] Interrupted.")
    finally:
        stop_event.set()
        t.join(timeout=2.0)
        send_sock.close()

if __name__ == "__main__":
    main()