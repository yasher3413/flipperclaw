# FlipperClaw

**An open-source AI agent running on ESP32-S3 with a Flipper Zero as the physical UI.**

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v5.5+-blue.svg)](https://docs.espressif.com/projects/esp-idf/)
[![uFBT](https://img.shields.io/badge/uFBT-latest-green.svg)](https://github.com/flipperdevices/flipperzero-ufbt)

---

## Architecture

```
┌─────────────────────┐         ┌──────────────────────┐         ┌──────────────┐
│    Flipper Zero      │  UART   │      ESP32-S3         │  HTTPS  │   LLM API    │
│                      │◄──────►│                       │◄──────►│              │
│  128×64 OLED         │115200  │  ReAct agent loop     │  SSE   │  Anthropic   │
│  D-pad + OK + BACK   │        │  Streaming LLM parser │        │   OpenAI     │
│  NFC / Sub-GHz / IR  │        │  SPIFFS memory store  │        └──────────────┘
│  FlipperClaw.fap (C) │        │  WiFi + TLS (C++17)   │
└─────────────────────┘         └──────────────────────┘
         ▲                                 ▲
    Furi SDK (C11)                  ESP-IDF v5.5 + FreeRTOS
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
| Config interface            | Serial CLI (`fc> `) + NVS          | Config file                  |

---

## Features

- **Streaming LLM responses** — tokens appear on the Flipper screen as they arrive, no waiting
- **ReAct agent loop** — up to 5 tool call iterations per prompt, reasoning visible mid-stream
- **Persistent memory** — SOUL.md (personality), MEMORY.md (recalled facts), session history on SPIFFS
- **Tool use** — web search (Tavily/Brave), current time, remember, NFC read, Sub-GHz replay, IR send
- **NFC integration** — agent can request Flipper to read an NFC tag and reason about the bytes
- **Sub-GHz integration** — agent can trigger replay of captured `.sub` files
- **Serial CLI** — configure WiFi, API keys, model provider entirely via `fc> ` serial prompt
- **Dual LLM provider** — swap between Anthropic (claude-haiku-4-5) and OpenAI (gpt-4o-mini) at runtime
- **Heartbeat monitor** — Flipper detects ESP32 disconnect within 15 seconds

---

## Hardware Requirements

| Item                    | Specification                          | Approx. Cost |
|-------------------------|----------------------------------------|--------------|
| Flipper Zero            | Any firmware version                   | ~$170        |
| ESP32-S3 dev board      | ESP32-S3-DevKitC-1 (4 MB flash recommended) | ~$10   |
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
# flipperclaw
