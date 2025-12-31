# ChatForwarder

A MetaHookSv plugin that forwards game events, chat messages, console output, and system logs to a specified UDP address.

## Overview

This plugin acts as a passive "listener" that hooks into various GoldSrc engine messaging systems. It aggregates these streams and sends them via UDP to a remote observer (e.g., an external chat bot, logger, or web interface).

**Note:** This plugin does **not** filter duplicate messages. Depending on the server type (Local vs. Dedicated) and engine behavior, the same text may appear in multiple streams (e.g., a Chat message appears in both `SayText` and `OutputDebugString`). The receiving end is responsible for deduplication if required.

## Protocol

The UDP packet structure is simple:
`[1 Byte Header] + [Message Body (UTF-8)]`

### Message Types & Hooks

| Header | Hex  | Source Hook | Description |
| :--- | :--- | :--- | :--- |
| **CHAT** | `0x12` | `UserMsg: SayText` | In-game chat. Reconstructs the string from arguments (e.g., `#Cstrike_Chat_All`). |
| **GAME** | `0x13` | `UserMsg: TextMsg` | Game notifications (e.g., "Game Commencing", Radio commands). |
| **NET** | `0x14` | `cl_parsefunc: print` | Raw console output received from the server. |
| **SYS** | `0x15` | `IAT: OutputDebugStringA` | Internal engine system logs and debug output. |
| **STUFF**| `0x16` | `cl_parsefunc: stufftext` | Server-to-Client command requests. |

### String Handling
Incoming strings are processed by a `CleanMessage` function before sending:
*   Standard control characters (0x00-0x1F) are stripped, **except** for:
    *   `0x01` to `0x04` (GoldSrc color codes) - Preserved.
    *   `\n`, `\r`, `\t` - Preserved.
*   The plugin adds its own Header Byte (`0x12` for Chat) at the very beginning of the packet. If the game string also starts with `0x02` (color code), the packet will look like `12 02 ...`. This is intentional behavior.

## Installation

1.  Copy `ChatForwarder.dll` (and `ChatForwarder_AVX2.dll` if supported) to your `svencoop/metahook/plugins/` directory.
2.  Add `ChatForwarder.dll` to your `plugins.lst`.

## Configuration (CVars)

| CVar | Default | Description |
| :--- | :--- | :--- |
| `cf_enabled` | `1` | Master switch. 0 = Disabled, 1 = Enabled. |
| `cf_server_ip` | `127.0.0.1` | The target IP address to send UDP packets to. |
| `cf_server_port` | `26000` | The target port to send UDP packets to. |
| `cf_listen_port` | `26001` | The local port to listen for incoming commands (UDP -> Game Console). |
| `cf_debug` | `0` | If `1`, prints sent messages to the game console for debugging. |
| `cf_listen_only` | `0` | If `1`, disables sending and only listens for incoming commands. |

## Build

The project is configured for **Visual Studio 2026** (v143/v145 toolset).

### Automated Build (PowerShell)
A standalone build script is included to compile the project without needing the full MetaHook solution structure.

```powershell
# Build Release and Release_AVX2
.\build_plugin.ps1

# Build and create a ZIP archive in /Builds
.\build_plugin.ps1 -Package

# Build and copy to game directory
.\build_plugin.ps1 -CopyTo "C:\Games\SvenCoop\metahook\plugins"
```

## Tools

*   `udp_test_client.py`: A simple Python script (no dependencies required) to act as a receiver/sender for testing the plugin.

```