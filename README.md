# ChatForwarder

A [MetaHookSv](https://github.com/hzqst/MetaHookSv) plugin for GoldSrc (Sven Co-op, Half-Life) that forwards game events, chat messages, and console output to a configurable UDP address. Also accepts remote console commands via UDP.

## Overview

ChatForwarder hooks into several GoldSrc engine messaging systems and aggregates their output into a single UDP stream. A remote observer — a chat bot, logger, dashboard, or automation script — simply listens on a UDP port and reacts to tagged messages.

**Typical use case:** React to in-game chat, player join/leave events (`... has joined the game`), and server notifications from an external script.

> **Note:** The same text may appear in multiple streams depending on engine behavior (e.g., a chat message can appear in both `SayText` and `OutputDebugString`). Deduplication, if needed, is the responsibility of the receiving end.

---

## Protocol

Every UDP packet has the structure:

```
[1 Byte Tag] [8 Bytes SteamID64 LE] [Message Body (UTF-8)]
```

### Message Tags

| Tag | Hex | Source Hook | Description |
|:----|:----|:------------|:------------|
| `PING` | `0x10` | Internal (`cf_ping`) | Diagnostics ping probe (`ping <nonce>`). Old servers ignore this tag. |
| `CHAT` | `0x12` | `UserMsg: SayText` | In-game chat. Arguments (`%s`) substituted inline. Carries the author's SteamID64. |
| `GAME` | `0x13` | `UserMsg: TextMsg` | Game notifications — includes join/leave events, radio, etc. |
| `NET`  | `0x14` | `cl_parsefunc: print` | Raw console output received from the server. |
| `SYS`  | `0x15` | `IAT: OutputDebugStringA` | Internal engine debug/system log output. |
| `STUFF`| `0x16` | `cl_parsefunc: stufftext` | Server-to-client command strings. |

Unknown tag bytes should be ignored by the client to maintain forward compatibility.

Delivery is plain UDP: there are no sequence numbers or acknowledgements, so packets can be lost, duplicated or arrive out of order.

### Packet Layout

The binary payload is always:

```text
[0]     tag_byte   : uint8   - message type
[1..8]  steamid64  : uint64  - little-endian SteamID64 (chat author, or 0)
[9..]   message    : bytes   - UTF-8 message body, not null-terminated
```

Parsing example:

```python
tag = buf[0]
steamid = struct.unpack_from("<Q", buf, 1)[0]
text = buf[9:].decode("utf-8", errors="replace")
```

`steamid == 0` means no SteamID is available: Steam is unavailable or not logged in, a LAN game, a bot, the engine did not provide the value, or it is not a valid individual account. For `CHAT` (`0x12`) the field is the message author's SteamID64 (read from the engine's player table on Sven Co-op, falling back to the standard `hud_player_info_t`), with the local player's ID as a fallback for your own messages. All other packet types use `0`. Availability depends on the game and engine version; check with `cf_debug 1`.

### String Handling

Incoming strings are processed by `CleanMessage` before sending:
- Control characters `0x00–0x1F` are stripped, **except**:
  - `0x01–0x04` — GoldSrc color codes, preserved.
  - `\n`, `\r`, `\t` — preserved.
- The tag byte is prepended **before** the cleaned string. If the game string itself starts with `0x02` (player name color), the packet will look like `12 02 ...` — this is intentional.
- The SteamID64 field is always 8 bytes (0 when not applicable).

### Inbound Commands (UDP → Console)

Any UTF-8 string sent to `cf_listen_port` is injected into the game console as a command. Commands are executed in the main game thread via `HUD_Frame` with an optional delay between them (`cf_command_delay`).

There is no authentication. Any host that can reach `cf_listen_port` can run console commands. `cf_allow_ip` compares the packet's source address only, and UDP source addresses can be spoofed. If you only need chat, set `cf_say_only 1`; if the peer runs on the same PC, set `cf_bind_ip 127.0.0.1`.

```
# Example: send a console command to the game
echo "say Hello from Python!" | nc -u 127.0.0.1 26001
```

---

## Installation

1. Copy `ChatForwarder.dll` (and `ChatForwarder_AVX2.dll` / `ChatForwarder_AVX.dll` if applicable) to your `svencoop/metahook/plugins/` directory.
2. Add `ChatForwarder.dll` to `svencoop/metahook/configs/plugins.lst`.

MetaHookSv automatically selects the most capable variant (`AVX2 → AVX → SSE2 → SSE → base`) for the current CPU.

---

## Configuration (CVars)

All CVars are saved to config (`FCVAR_ARCHIVE`). Changes are picked up about once per second; `cf_reload` re-reads them right away and recreates the sockets.

| CVar | Default | Description |
|:-----|:--------|:------------|
| `cf_enabled` | `1` | Master switch. `0` = plugin idle (no send, no receive). |
| `cf_bind_ip` | `0.0.0.0` | Local IPv4 address to bind the listener and sender sockets to (`0.0.0.0` for all interfaces, or a specific LAN IP). |
| `cf_server_ip` | `127.0.0.1` | Target IP address for outgoing UDP messages. |
| `cf_server_port` | `26000` | Target port for outgoing UDP messages. |
| `cf_listen_port` | `26001` | Local UDP port for incoming console commands. |
| `cf_allow_ip` | `*` | Accept packets only from this source IP (`*` = any). Compared against the packet's source address, which can be spoofed. A peer on the same machine that sends to a LAN IP arrives from that LAN IP, not `127.0.0.1`. |
| `cf_say_only` | `0` | If `1`: accept only `say ` and `say_team ` commands. Commands containing `;`, CR, LF or NUL are rejected. |
| `cf_listen_only` | `0` | If `1`: only receive commands, do **not** send any messages. |
| `cf_command_delay` | `0` | Minimum seconds between consecutive console command executions. |
| `cf_debug` | `0` | If `1`: print forwarded messages, network debug info, and client SteamIDs to the in-game console. |

---

## Console Commands

Diagnostic and management commands available in the game console:

| Command | Description |
|:--------|:------------|
| `cf_status` | Prints detailed status: listener and sender socket states, OS routing probe to `cf_server_ip`, packet counters (RX, TX, filtered, denied), pong count, RTT, server-seen address, and configuration warnings. |
| `cf_ifaces` | Lists all local IPv4 network interfaces via `SIO_GET_INTERFACE_LIST`, marking the one used for the route to `cf_server_ip`. |
| `cf_ping` | Queues a diagnostic ping probe (`0x10`) to `cf_server_ip:cf_server_port`. A pong-capable peer answers with `0x1F PONG <nonce> <ip>:<port>`, updating RTT and peer-seen address. |
| `cf_reload` | Re-reads all CVars, recreates the listener and sender sockets, and resets statistics counters. |

---

## Build

The project targets **Visual Studio 2022+** (v143/v145 toolset), 32-bit Release.

### Automated Build (PowerShell)

A standalone build script is included — no need for the full MetaHookSv solution.

```powershell
# Build Release and Release_AVX2
.\build_plugin.ps1

# Build and package into a ZIP archive in /Builds
.\build_plugin.ps1 -Package

# Build and copy directly to game directory
.\build_plugin.ps1 -CopyTo "C:\Games\SvenCoop\svencoop\metahook\plugins"
```

---

## Tools

**`udp_test_client.py`** — Reference Python 3 UDP test client and connection debugger.

- **Interface Selection:** Lists available network interfaces on startup or accepts `-b / --bind <IP>`.
- **Target Routing:** Sends interactive console commands to the plugin target address (`-s / --server`, default `127.0.0.1`, `-sp / --sport`, default `26001`).
- **Ping / Pong:** Responds to diagnostic ping probes (`0x10`) with `0x1F PONG <nonce> <src_ip>:<src_port>`, allowing `cf_status` to measure RTT.
- **Display & Formatting:** Renders GoldSrc color codes (`0x01–0x04`) as ANSI terminal colors and suppresses duplicate packets within a 200 ms window (`--show-dups` to disable).

```bash
# Local testing (default 127.0.0.1)
python udp_test_client.py

# LAN testing
python udp_test_client.py -b 192.168.0.15 -s 192.168.0.15 -sp 26001 -lp 26000
```

---

## Architecture Notes

- Worker tasks run via the MetaHookSv global thread pool (`GetGlobalThreadPool`).
- Outgoing packets are queued and sent asynchronously.
- Inbound UDP commands are queued and executed on the main engine thread in `HUD_Frame`.
- `OutputDebugStringA` IAT hook captures engine debug log output with line buffering.
- `HUD_GetStudioModelInterface` is wrapped only to read the engine's player table; the original is always called.
- Engine hooks are registered once on initial load.
