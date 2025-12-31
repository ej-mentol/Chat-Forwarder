import socket
import threading
import sys
import time
import datetime
import os

# ==============================================================================
# CONFIGURATION
# ==============================================================================
SERVER_IP = "127.0.0.1"
# Port to SEND commands TO (Must match 'cf_listen_port' in game)
# Default: 26001
SEND_PORT = 26001

# Port to LISTEN for messages FROM (Must match 'cf_server_port' in game)
# Default: 26000
LISTEN_PORT = 26000
SHOW_TYPES = []

# ==============================================================================
# CONSTANTS & MAPPING
# ==============================================================================
TAG_MAP = {
    0x12: "[CHAT]",
    0x13: "[GAME]",
    0x14: "[NET]",
    0x15: "[SYS]",
    0x16: "[STUFF]"
}

# ANSI Colors
ANSI_RESET  = "\033[0m"
ANSI_NORMAL = "\033[0m"       # 0x01
ANSI_NAME   = "\033[1;36m"    # 0x02 (Bold Cyan)
ANSI_TEAM   = "\033[33m"      # 0x03 (Yellow)
ANSI_GREEN  = "\033[32m"      # 0x04 (Green)
ANSI_GREY   = "\033[90m"      # Timestamp

def enable_ansi_windows():
    """Enable ANSI escape sequences on Windows 10+ conhost"""
    if os.name == 'nt':
        from ctypes import windll, c_int, byref
        stdout_handle = windll.kernel32.GetStdHandle(c_int(-11))
        mode = c_int(0)
        windll.kernel32.GetConsoleMode(stdout_handle, byref(mode))
        mode.value |= 0x0004
        windll.kernel32.SetConsoleMode(stdout_handle, mode)

def parse_goldsrc_colors(data_bytes):
    """
    Converts GoldSrc bytes (0x01-0x04) to ANSI escape codes while preserving UTF-8.
    """
    res_bytes = bytearray()
    
    # ANSI sequences as bytes
    mapping = {
        0x01: ANSI_NORMAL.encode('utf-8'),
        0x02: ANSI_NAME.encode('utf-8'),
        0x03: ANSI_TEAM.encode('utf-8'),
        0x04: ANSI_GREEN.encode('utf-8'),
    }
    
    for byte in data_bytes:
        if byte in mapping:
            res_bytes.extend(mapping[byte])
        elif byte == 0x0A: # Newline
            res_bytes.extend(b"\n")
        elif byte == 0x0D: # CR
            pass
        elif byte >= 0x20 or byte > 0x7F: # Printable or UTF-8 lead/trail bytes
            res_bytes.append(byte)
            
    try:
        return res_bytes.decode('utf-8', errors='replace') + ANSI_RESET
    except Exception:
        return res_bytes.decode('latin-1', errors='replace') + ANSI_RESET

def listener_thread(stop_event):
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
            if not data: continue

            tag_byte = data[0]
            payload = data[1:]

            if SHOW_TYPES and tag_byte not in SHOW_TYPES:
                continue

            timestamp = f"{ANSI_GREY}[{datetime.datetime.now().strftime('%H:%M:%S')}]{ANSI_RESET}"
            
            # Determine prefix color based on tag
            tag_label = TAG_MAP.get(tag_byte, f"[UNK:0x{tag_byte:02X}]")
            
            # Apply GoldSrc color parsing to the payload
            formatted_message = parse_goldsrc_colors(payload)

            # Construct final string
            # Example: [12:00:00] [CHAT] PlayerName : hello
            print(f"{timestamp} {tag_label} {formatted_message}")

        except socket.timeout:
            continue
        except Exception as e:
            print(f"[ERROR] Receive error: {e}")

    sock.close()
    print("[INFO] Listener stopped.")

def main():
    enable_ansi_windows()
    
    print("=================================================")
    print(" ChatForwarder UDP Test Client")
    print(f" Target:   {SERVER_IP}:{SEND_PORT}")
    print(f" Listening: {LISTEN_PORT}")
    print("=================================================")
    
    stop_event = threading.Event()
    t = threading.Thread(target=listener_thread, args=(stop_event,), daemon=True)
    t.start()

    send_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    try:
        while True:
            cmd = input()
            if cmd.lower() in ['quit', 'exit']: break
            if not cmd.strip(): continue

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