![flipperclaw_logo_v3](https://github.com/user-attachments/assets/ab032a95-fd31-4fb5-a670-312d7c6b2b41)

**An open-source AI agent running on ESP32-S3 with a Flipper Zero as the physical UI.**

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v5.5+-blue.svg)](https://docs.espressif.com/projects/esp-idf/)
[![uFBT](https://img.shields.io/badge/uFBT-latest-green.svg)](https://github.com/flipperdevices/flipperzero-ufbt)

---

## Architecture

```
┌─────────────────────┐        ┌──────────────────────┐        ┌──────────────┐
│    Flipper Zero     │  UART  │      ESP32-S3        │ HTTPS  │   LLM API    │
│                     │◄──────►│                      │◄──────►│              │
│  128×64 OLED        │ 115200 │  ReAct agent loop    │  SSE   │  Anthropic   │
│  D-pad + OK + BACK  │        │  Streaming LLM parser│        │   OpenAI     │
│  NFC / Sub-GHz / IR │        │  SPIFFS memory store │        └──────────────┘
│  FlipperClaw.fap (C)│        │  WiFi + TLS (C++17)  │
└─────────────────────┘        └──────────────────────┘
         ▲                                 ▲
    Furi SDK (C11)               ESP-IDF v5.5 + FreeRTOS
```

---

## What Makes FlipperClaw Different from MimiClaw?

| Feature                     | FlipperClaw                        | MimiClaw                     |
|-----------------------------|------------------------------------|------------------------------|
| UI hardware                 | Flipper Zero (128×64 + d-pad)      | Serial terminal              |
| Transport                   | UART with Base64 framing           | USB serial                   |
| Hardware integration        | NFC, Sub-GHz, IR via agent tools   | None                         |
| ESP32 language              | C++17 (ESP-IDF v5.5)               | Varies                       |
| Flipper language            | C11 (Furi SDK, uFBT)               | N/A                          |
| Agent tool: NFC read        | Yes — `flipper_nfc_read` tool      | No                           |
| Agent tool: Sub-GHz replay  | Yes — `flipper_subghz_replay` tool | No                           |
| User profile (USER.md)      | Yes — agent knows who you are      | No                           |
| Task queue (HEARTBEAT.md)   | Yes — auto-checked every 30 min    | No                           |
| Daily notes                 | Yes — per-day SPIFFS log           | No                           |
| Cron scheduler              | Yes — recurring + one-shot jobs    | No                           |
| Config interface            | Serial CLI (`fc> `) + NVS          | Config file                  |

---

## Features

- **Streaming LLM responses** — tokens appear on the Flipper screen as they arrive, no waiting
- **ReAct agent loop** — up to 5 tool call iterations per prompt, reasoning visible mid-stream
- **Persistent memory** — SOUL.md (personality), MEMORY.md (recalled facts), session history on SPIFFS
- **User profile** — USER.md on SPIFFS; describe yourself once and the agent always knows who you are
- **Daily notes** — agent reads and appends to `YYYY-MM-DD.md` files; a running log of every session
- **HEARTBEAT.md task queue** — add tasks in Markdown checklist format; the agent wakes every 30 minutes, acts on pending items, and marks them done with `- [x]`
- **Cron scheduler** — schedule recurring or one-shot prompts that fire automatically while you're away; persisted to `cron.json` across reboots
- **Tool use** — web search (Tavily/Brave), current time (syncs ESP32 clock from worldtimeapi.org), remember, NFC read, Sub-GHz replay, IR send, cron management
- **NFC integration** — agent can request Flipper to read an NFC tag and reason about the bytes
- **Sub-GHz integration** — agent can trigger replay of captured `.sub` files
- **Serial CLI** — configure WiFi, API keys, model provider entirely via `fc> ` serial prompt
- **Dual LLM provider** — swap between Anthropic (claude-haiku-4-5) and OpenAI (gpt-4o-mini) at runtime
- **Heartbeat monitor** — Flipper detects ESP32 disconnect within 15 seconds

---

## Memory & Storage

All files live on the ESP32's SPIFFS partition (`/spiffs/`):

| File              | Purpose                                                           |
|-------------------|-------------------------------------------------------------------|
| `SOUL.md`         | Agent personality and system instructions (edit freely)          |
| `USER.md`         | Your personal profile — role, preferences, context               |
| `MEMORY.md`       | Facts the agent has remembered via the `remember` tool           |
| `HEARTBEAT.md`    | Markdown checklist of pending tasks; checked every 30 minutes    |
| `session.jsonl`   | Rolling conversation history (last N turns)                      |
| `YYYY-MM-DD.md`   | Daily notes — one file per day, auto-appended each session       |
| `cron.json`       | Persisted cron jobs (survives reboots)                           |

---

## Agent Tools

| Tool                      | Description                                                       |
|---------------------------|-------------------------------------------------------------------|
| `web_search`              | Search the web via Tavily or Brave API                            |
| `get_current_time`        | Fetch UTC time from worldtimeapi.org and sync the ESP32 clock     |
| `remember`                | Append a fact to MEMORY.md for future sessions                    |
| `flipper_nfc_read`        | Ask the Flipper to scan an NFC tag and return the raw bytes       |
| `flipper_subghz_replay`   | Trigger replay of a captured Sub-GHz signal file                  |
| `flipper_ir_send`         | Send an IR command via the Flipper's IR blaster                   |
| `cron_add`                | Schedule a recurring or one-shot prompt                           |
| `cron_list`               | List all pending cron jobs                                        |
| `cron_remove`             | Cancel a cron job by ID                                           |

---

## Hardware Requirements

| Item                    | Specification                          | Approx. Cost |
|-------------------------|----------------------------------------|--------------|
| Flipper Zero            | Any firmware version                   | ~$170        |
| ESP32-S3 dev board      | ESP32-S3-DevKitC-1 (4 MB flash recommended) | ~$10    |
| Jumper wires            | 4× female-to-female                    | ~$2          |
| **Total (budget)**      |                                        | **~$182**    |

Or use the official [Flipper Zero WiFi Devboard](https://shop.flipperzero.one/products/wifi-devboard)
(ESP32-S2, ~$30) — note it uses ESP32-S2 (single-core, no PSRAM), not S3.

---

## Quick Start

```bash
# 1. Clone
git clone https://github.com/yasher3413/flipperclaw.git
cd flipperclaw

# 2. Build ESP32 firmware
cp esp32/main/fc_secrets.h.example esp32/main/fc_secrets.h
# Edit fc_secrets.h with your WiFi SSID, password, and Anthropic API key
cd esp32 && ./scripts/build.sh && ./scripts/flash.sh /dev/ttyUSB0

# 3. Build Flipper app
cd ../flipper && ufbt
# Copy dist/flipperclaw.fap to Flipper SD card: apps/GPIO/flipperclaw.fap
```

See [docs/SETUP.md](docs/SETUP.md) for full wiring diagrams and configuration steps.

---

## Configuration

### fc_secrets.h (compile-time defaults)

```cpp
#define FC_SECRET_WIFI_SSID        "YourWiFiName"
#define FC_SECRET_WIFI_PASS        "YourWiFiPassword"
#define FC_SECRET_API_KEY          "sk-ant-api03-xxxxx"
#define FC_SECRET_MODEL_PROVIDER   "anthropic"
#define FC_SECRET_TAVILY_KEY       ""
#define FC_SECRET_BRAVE_KEY        ""
```

### Serial CLI (runtime, stored in NVS)

Connect via `screen /dev/ttyUSB0 115200` and use the `fc> ` prompt:

| Command                             | Description                          |
|-------------------------------------|--------------------------------------|
| `wifi_set <ssid> <pass>`            | Set WiFi credentials                 |
| `set_api_key <key>`                 | Set LLM API key                      |
| `set_model_provider <anthropic\|openai>` | Switch LLM provider             |
| `set_model <name>`                  | Set model name                       |
| `set_tavily_key <key>`              | Web search via Tavily                |
| `config_show`                       | Print all settings                   |
| `status`                            | Show WiFi/heap/UART state            |
| `restart`                           | Reboot ESP32                         |

---

## Protocol

FlipperClaw uses a simple newline-framed, Base64-encoded ASCII protocol over UART. Any alternative
UI (desktop app, BLE bridge, web interface) can implement it from the spec alone.

See [docs/PROTOCOL.md](docs/PROTOCOL.md) for the full wire format, timing diagrams, and error
handling tables.

---

## Contributing

Issues and pull requests are welcome. Please:
1. Open an issue before large PRs to discuss the approach.
2. Follow existing code style (C++17 for ESP32, C11 for Flipper).
3. If implementing an alternative UI or transport, refer to `docs/PROTOCOL.md` — the wire protocol
   is the stable interface.

---

## License

MIT — see [LICENSE](LICENSE).
