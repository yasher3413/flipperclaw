# FlipperClaw Architecture

## System Overview

```
┌──────────────────────────────────────────────────────────────────┐
│                        FlipperClaw System                        │
│                                                                  │
│  ┌─────────────────────────┐       ┌────────────────────────┐   │
│  │      Flipper Zero        │       │       ESP32-S3          │   │
│  │                          │ UART  │                        │   │
│  │  ┌────────────────────┐  │◄─────►│  ┌──────────────────┐ │   │
│  │  │  FlipperClaw .fap  │  │115200 │  │   agent.cpp      │ │   │
│  │  │  (C11, Furi SDK)   │  │       │  │   (ReAct loop)   │ │   │
│  │  ├────────────────────┤  │       │  ├──────────────────┤ │   │
│  │  │  ui_chat.c         │  │       │  │   llm_api.cpp    │ │   │
│  │  │  ui_input.c        │  │       │  │   (HTTPS SSE)    │ │   │
│  │  │  ui_status.c       │  │       │  ├──────────────────┤ │   │
│  │  ├────────────────────┤  │       │  │   tools.cpp      │ │   │
│  │  │  uart_bridge.c     │  │       │  │   (web/hw tools) │ │   │
│  │  │  (base64, framing) │  │       │  ├──────────────────┤ │   │
│  │  ├────────────────────┤  │       │  │  memory_store.cpp│ │   │
│  │  │  Furi HAL UART     │  │       │  │  (SPIFFS)        │ │   │
│  │  │  NFC / SubGHz / IR │  │       │  ├──────────────────┤ │   │
│  │  └────────────────────┘  │       │  │  wifi_client.cpp │ │   │
│  │                          │       │  │  uart_proto.cpp  │ │   │
│  │  128×64 mono OLED        │       │  │  cli.cpp (USB)   │ │   │
│  │  D-pad + OK + BACK       │       │  └──────────────────┘ │   │
│  └─────────────────────────┘       │                        │   │
│                                     │   HTTPS ▲   SPIFFS ▼  │   │
│                                     └─────────┼─────────────┘   │
│                                               │                  │
│                                    ┌──────────▼──────────┐      │
│                                    │   LLM API (Cloud)    │      │
│                                    │  Anthropic / OpenAI  │      │
│                                    └─────────────────────┘      │
└──────────────────────────────────────────────────────────────────┘
```

---

## Why Split-Brain Architecture?

The Flipper Zero is purpose-built for portability and hardware interaction. Its STM32WB55 MCU has
256 KB of RAM and runs Furi OS — a lean RTOS optimised for battery life and radio hardware. It
cannot run TLS, make HTTPS calls, or maintain a large conversational context buffer.

The ESP32-S3 fills those gaps: it has 512 KB of internal SRAM (plus optional 8 MB PSRAM), hardware
acceleration for AES/SHA, and a dual-core Xtensa LX7 at 240 MHz. It can hold an LLM conversation
history, stream HTTPS responses, and run a full FreeRTOS task scheduler — all at ~$10–30 in
hardware cost.

Together:
- **Flipper = UI + hardware peripherals**: NFC, Sub-GHz, IR, display, d-pad
- **ESP32 = compute + network**: WiFi, TLS, LLM streaming, persistent storage

UART connects them with a simple, auditable protocol that an alternative UI (BLE app, desktop) can
implement independently by reading `docs/PROTOCOL.md`.

---

## Data Flow

```
User presses OK on d-pad
        │
        ▼
[ui_input.c — character picker]
        │  uart_send_prompt()
        ▼
[uart_bridge.c — base64 encode, write USART1]
        │  115200 baud
        ▼
[uart_proto.cpp — rx_task parses PROMPT line]
        │  dispatch callback
        ▼
[agent.cpp — run(prompt)]
  ├── append to history vector
  ├── load SOUL.md from SPIFFS
  ├── build messages array (ArduinoJson)
  │
  ▼
[llm_api.cpp — stream()]
  ├── HTTP POST to Anthropic/OpenAI
  ├── parse SSE line-by-line
  │     └── content_block_delta → token_cb()
  │
  ▼
[agent.cpp — token_cb]
  └── UartProto::send("CHUNK", token)
        │  base64 encode
        ▼
[uart_bridge.c — rx_thread parses CHUNK]
        │  post AppEvent to queue
        ▼
[ui_chat.c — draw_callback]
        └── append to response buffer, scroll, re-render
```

---

## Memory Budget

### Flipper Zero (STM32WB55, 256 KB RAM)

| Region             | Budget     |
|--------------------|------------|
| Furi OS kernel     | ~80 KB     |
| Other running apps | ~60 KB     |
| FlipperClaw FAP    | 4 KB stack |
| Response buffer    | 2 KB       |
| UART rx buffer     | 512 B      |
| Event queue        | ~1 KB      |
| **Total FAP**      | **~8 KB**  |

The 2 KB response buffer (`response_buf[2048]`) is the largest allocation. The Flipper never stores
conversation history — that lives on the ESP32.

### ESP32-S3 (512 KB SRAM + 1 MB SPIFFS)

| Region                   | Budget      |
|--------------------------|-------------|
| FreeRTOS kernel + tasks  | ~50 KB      |
| WiFi/TLS stack           | ~80 KB      |
| UART rx/tx buffers       | 8 KB        |
| Conversation history     | ~50 KB max  |
| LLM streaming buffer     | 16 KB       |
| ArduinoJson docs         | ~8 KB       |
| **SPIFFS (1 MB)**        |             |
| SOUL.md                  | ~1 KB       |
| session.jsonl            | ~50 KB max  |
| MEMORY.md                | ~10 KB      |

---

## Language Choices

### C++17 on ESP32

Justified by toolchain maturity and library ecosystem:
- `std::string` and `std::vector` give safe dynamic buffers without manual allocations.
- `std::function<>` enables clean callback patterns across the streaming pipeline.
- `std::atomic<bool>` for the cancel flag is race-free without custom mutexes.
- ArduinoJson v7 is header-only and integrates cleanly via idf_component_manager.
- Binary size overhead vs. C is negligible on a 4 MB flash device.

### C11 on Flipper

The Furi SDK is written in C and exposes a C API only. There is no C++ ABI compatibility layer —
attempting to link C++ objects against Furi primitives causes linker failures. FlipperClaw's `.fap`
therefore uses C11 throughout: structs, function pointers, and manual memory management with
`furi_alloc`/`free`.

---

## Hardware Integration Design

The agent controls Flipper hardware via `HW:` command messages over UART. The pattern is:

1. Agent calls a tool (e.g. `flipper_nfc_read`).
2. Tools layer sends `HW:NFC:READ\n` over UART.
3. Flipper receives it, activates the hardware, waits for user interaction.
4. Flipper sends result back as `HW:NFC:DATA:<base64>\n`.
5. Tools layer decodes, formats, and returns result to the agent.
6. Agent continues the ReAct loop with the hardware result in context.

This design keeps hardware logic on the Flipper (where the drivers live) and decision logic on the
ESP32 (where the LLM context lives), with UART as a clean RPC boundary.

---

## FreeRTOS Task Layout

| Task            | Core | Stack  | Priority | Description                        |
|-----------------|------|--------|----------|------------------------------------|
| `uart_rx_task`  | 0    | 4 KB   | 5        | Read UART1 bytes, parse lines      |
| `wifi_task`     | 0    | 4 KB   | 4        | WiFi connect / reconnect loop      |
| `cli_task`      | 0    | 4 KB   | 3        | USB serial CLI (UART0)             |
| `heartbeat_task`| 0    | 2 KB   | 2        | Send STATUS pings                  |
| `agent_task`    | 1    | 8 KB   | 5        | ReAct loop + tool dispatch         |

Agent and LLM work are pinned to core 1 to avoid contention with the WiFi stack (core 0). The UART
RX task is on core 0 to share interrupt affinity with UART hardware.

---

## Future Expansion

- **BLE transport**: Replace UART with BLE SPP for wireless operation. Protocol is unchanged — only
  the physical transport layer needs a swap in `uart_proto.cpp`.
- **Alternative UIs**: Any device that speaks the wire protocol (see `docs/PROTOCOL.md`) can drive
  the ESP32 agent. A desktop CLI, a web UI over WebSockets, or a custom PCB could all be drop-in
  Flipper replacements.
- **Phase 2 hardware**: Full NFC read/write (currently stubbed), Sub-GHz capture and replay via
  `flipper_subghz_replay`, IR blaster via `flipper_ir_send`.
- **Local LLM**: Replace `llm_api.cpp` backend with llama.cpp server running on a local machine.
  The agent loop is provider-agnostic — only the API URL and auth header change.
