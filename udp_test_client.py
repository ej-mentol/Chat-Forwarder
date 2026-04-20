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

# Deduplication: suppress messages with identical text content arriving within
# this window. Handles loopback where the same event fires in both [SYS]
# (OutputDebugString) and [CHAT]/[GAME] (UserMsg) simultaneously.
# Set DEDUP_WINDOW to 0 to disable.
DEDUP_ENABLED = True
DEDUP_WINDOW  = 0.25   # seconds

# Only tags listed here can be silently dropped as duplicates.
# CHAT/GAME/NET/STUFF are never suppressed — they are the "authoritative" source.
# SYS (0x15) is suppressed because it echoes almost everything the engine does.
DEDUP_SUPPRESS_TAGS = {0x15}  # SYS only

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

def is_duplicate(tag_byte: int, payload: bytes) -> bool:
    """
    Returns True only if:
      1. The same content was registered in the dedup buffer recently, AND
      2. This tag type is in DEDUP_SUPPRESS_TAGS.

    All tags register their content so they can suppress future [SYS] arrivals,
    but only tags in DEDUP_SUPPRESS_TAGS are ever dropped themselves.

    This means:
      SYS arrives first  → registered → shown
      CHAT arrives 2ms later → found in buf → NOT in suppress list → shown ✓
      SYS arrives again  → found in buf → in suppress list → DROPPED ✓

      CHAT arrives first → registered → shown
      SYS arrives 2ms later → found in buf → in suppress list → DROPPED ✓
    """
    if not DEDUP_ENABLED or DEDUP_WINDOW <= 0:
        return False

    now = time.monotonic()
    key = _content_key(payload)

    with _dedup_lock:
        # Evict expired entries
        while _dedup_buf and now - _dedup_buf[0][0] > DEDUP_WINDOW:
            _dedup_buf.popleft()

        found = any(k == key for _, k in _dedup_buf)

        # Always register if not already present (so future SYS can be suppressed)
        if not found:
            _dedup_buf.append((now, key))

        # Only suppress if the content was seen before AND this tag is suppressible
        return found and (tag_byte in DEDUP_SUPPRESS_TAGS)

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

            # Deduplication: SYS is dropped if the same content already
            # arrived (or will arrive) from a higher-priority tag.
            if is_duplicate(tag_byte, payload):
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