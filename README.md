# ChatForwarder

A [MetaHookSv](https://github.com/hzqst/MetaHookSv) plugin for GoldSrc (Sven Co-op, Half-Life) that forwards game events, chat messages, and console output to a configurable UDP address. Also accepts remote console commands via UDP.

## Overview

ChatForwarder hooks into several GoldSrc engine messaging systems and aggregates their output into a single UDP stream. A remote observer — a chat bot, logger, dashboard, or automation script — simply listens on a UDP port and reacts to tagged messages.

**Typical use case:** React to in-game chat, player join/leave events (`... has joined the game`), and server notifications in real time from an external script.

> **Note:** The same text may appear in multiple streams depending on engine behavior (e.g., a chat message can appear in both `SayText` and `OutputDebugString`). Deduplication, if needed, is the responsibility of the receiving end.

---

## Protocol

Every UDP packet has the structure:

```
[1 Byte Tag] [Message Body (UTF-8)]
```

### Message Tags

| Tag | Hex | Source Hook | Description |
|:----|:----|:------------|:------------|
| `CHAT` | `0x12` | `UserMsg: SayText` | In-game chat. Arguments (`%s`) are substituted inline. |
| `GAME` | `0x13` | `UserMsg: TextMsg` | Game notifications — includes join/leave events, radio, etc. |
| `NET`  | `0x14` | `cl_parsefunc: print` | Raw console output received from the server. |
| `SYS`  | `0x15` | `IAT: OutputDebugStringA` | Internal engine debug/system log output. |
| `STUFF`| `0x16` | `cl_parsefunc: stufftext` | Server-to-client command strings. |

Unknown tag bytes should be ignored by the client to maintain forward compatibility.

### String Handling

Incoming strings are processed by `CleanMessage` before sending:
- Control characters `0x00–0x1F` are stripped, **except**:
  - `0x01–0x04` — GoldSrc color codes, preserved.
  - `\n`, `\r`, `\t` — preserved.
- The tag byte is prepended **before** the cleaned string. If the game string itself starts with `0x02` (player name color), the packet will look like `12 02 ...` — this is intentional.

### Inbound Commands (UDP → Console)

Any UTF-8 string sent to `cf_listen_port` is injected into the game console as a command. Commands are executed in the main game thread via `HUD_Frame` with an optional delay between them (`cf_command_delay`).

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

All CVars are saved to config (`FCVAR_ARCHIVE`) and take effect immediately at runtime.

| CVar | Default | Description |
|:-----|:--------|:------------|
| `cf_enabled` | `1` | Master switch. `0` = plugin idle (no send, no receive). |
| `cf_server_ip` | `127.0.0.1` | Target IP address for outgoing UDP messages. |
| `cf_server_port` | `26000` | Target port for outgoing UDP messages. |
| `cf_listen_port` | `26001` | Local UDP port for incoming console commands. |
| `cf_listen_only` | `0` | If `1`: only receive commands, do **not** send any messages. Listener always runs; sender is disabled. |
| `cf_command_delay` | `0` | Minimum seconds between consecutive console command executions. |
| `cf_debug` | `0` | If `1`: print all forwarded messages to the in-game console. |

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

**`udp_test_client.py`** — A zero-dependency Python 3 script for testing the plugin.

- Listens on `LISTEN_PORT` (default `26000`) and prints tagged messages with ANSI color support.
- Accepts console input and sends it as commands to `SEND_PORT` (default `26001`).
- Handles GoldSrc color bytes (`0x01–0x04`) and maps them to terminal colors.

```
python udp_test_client.py
```

---

## Architecture Notes

- Uses the **MetaHookSv Global Thread Pool** (`GetGlobalThreadPool`) — no dedicated threads are created.
- Outgoing messages are batched via a lock-free-friendly `SendQueue` and dispatched by a dedicated sender work item.
- Inbound commands from UDP are queued and executed on the main thread in `HUD_Frame` to comply with GoldSrc's single-threaded console model.
- The `OutputDebugStringA` IAT hook on the engine module captures system-level log lines with line-buffering and a 4 KB safety flush.
- Hooks (`HookUserMsg`, `HookCLParseFuncByName`) are registered exactly once across all map loads.