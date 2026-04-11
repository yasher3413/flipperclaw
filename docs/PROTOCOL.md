# FlipperClaw UART Protocol Specification

## Overview

FlipperClaw uses a simple, robust ASCII-over-UART protocol to communicate between the Flipper Zero
and the ESP32-S3. All messages are newline-terminated (`\n`). All text payloads are Base64-encoded
(RFC 4648 standard) to safely transport arbitrary content — including newlines, Unicode, and binary
data — without escaping ambiguity.

**Baud rate:** 115200  
**Max message length:** 4096 bytes (including header, payload, and newline)  
**Flipper UART:** USART1 — TX on GPIO pin 13, RX on GPIO pin 14  
**ESP32-S3 UART:** UART1 — TX on GPIO 17, RX on GPIO 18 (configurable via sdkconfig)

---

## Message Format

```
<TYPE>[:<PAYLOAD>]\n
```

- `<TYPE>` is an ASCII identifier (uppercase, may contain `:` as sub-separator).
- `<PAYLOAD>` is a Base64-encoded string (no newlines within payload).
- Every message is terminated with a single `\n` character.
- If a type carries no payload, the trailing `:<PAYLOAD>` is omitted.

---

## Message Table

### Flipper Zero → ESP32

| Message                        | Direction      | Payload         | Description                                   |
|-------------------------------|----------------|-----------------|-----------------------------------------------|
| `PROMPT:<base64_text>\n`       | Flipper → ESP  | Base64 string   | User-entered prompt to send to LLM            |
| `CANCEL\n`                     | Flipper → ESP  | None            | Abort current inference mid-stream            |
| `PING\n`                       | Flipper → ESP  | None            | Heartbeat (sent every 10 seconds)             |
| `HW:NFC:DATA:<base64>\n`       | Flipper → ESP  | Base64 bytes    | NFC tag bytes read by the Flipper             |
| `HW:SUBGHZ:DATA:<base64>\n`    | Flipper → ESP  | Base64 bytes    | Sub-GHz capture data from Flipper             |

### ESP32 → Flipper Zero

| Message                              | Direction      | Payload         | Description                                        |
|-------------------------------------|----------------|-----------------|-----------------------------------------------------|
| `CHUNK:<base64_text>\n`              | ESP → Flipper  | Base64 string   | Streaming response fragment (sent as tokens arrive) |
| `DONE\n`                             | ESP → Flipper  | None            | Full response complete                              |
| `ERROR:<code>\n`                     | ESP → Flipper  | ASCII code      | Error code (see table below)                        |
| `PONG\n`                             | ESP → Flipper  | None            | Heartbeat reply                                     |
| `STATUS:<base64_text>\n`             | ESP → Flipper  | Base64 string   | Human-readable status update                        |
| `HW:NFC:READ\n`                      | ESP → Flipper  | None            | Agent requests Flipper to read an NFC tag           |
| `HW:SUBGHZ:REPLAY:<filename>\n`      | ESP → Flipper  | ASCII filename  | Agent requests replay of a .sub file               |
| `HW:IR:SEND:<filename>\n`            | ESP → Flipper  | ASCII filename  | Agent requests IR signal send                       |

---

## Error Codes

| Code        | Meaning                                          | Flipper Recovery                              |
|-------------|--------------------------------------------------|-----------------------------------------------|
| `NO_WIFI`   | ESP32 not connected to WiFi                      | Show error, retry on next prompt              |
| `API_ERR`   | LLM API returned an error (non-2xx HTTP)         | Show error message, allow retry               |
| `TIMEOUT`   | LLM API did not respond within timeout window    | Show error, allow retry                       |
| `PARSE_ERR` | Failed to parse SSE/JSON response from API       | Show error, allow retry                       |
| `CANCELLED` | Inference was cancelled by CANCEL message        | Return to idle state, show "Cancelled"        |

---

## Base64 Encoding Rationale

Raw text cannot be safely transmitted as-is because:
- LLM responses may contain `\n` characters, which would corrupt line framing.
- Responses may include Unicode, emoji, and non-ASCII bytes.
- Tool results may include binary data (NFC tag bytes, sub-GHz captures).

All text payloads are Base64-encoded before transmission and decoded upon receipt. The implementation
uses RFC 4648 standard Base64 with the `+` and `/` characters and `=` padding. Neither side should
attempt to interpret a payload before decoding.

---

## Example Exchange

```
[Flipper boots, sends PING]

Flipper → ESP:   PING\n
ESP → Flipper:   PONG\n

[Flipper sends PING every 10s; ESP replies with PONG.
 If no PONG received within 15s, Flipper marks connection as lost.]

[User types "What time is it?" and presses OK]

Flipper → ESP:   PROMPT:V2hhdCB0aW1lIGlzIGl0Pw==\n

[ESP receives PROMPT, starts processing]

ESP → Flipper:   STATUS:Q29ubmVjdGluZyB0byBMTE0uLi4=\n
                 ("Connecting to LLM...")

[ESP calls LLM API — streaming begins]

ESP → Flipper:   CHUNK:SXQ=\n           ("It")
ESP → Flipper:   CHUNK:IGlz\n           (" is")
ESP → Flipper:   CHUNK:IDM6MjI=\n       (" 3:22")
ESP → Flipper:   CHUNK:IFBNLg==\n       (" PM.")
ESP → Flipper:   DONE\n

[Flipper displays assembled response "It is 3:22 PM." on screen]

[User presses BACK — Flipper sends CANCEL (if mid-stream)]

Flipper → ESP:   CANCEL\n
ESP → Flipper:   ERROR:CANCELLED\n
```

---

## Timing Diagram

```
Flipper                              ESP32
   |                                   |
   |-------- PING ------------------>  |  (t=0s)
   |  <------- PONG ----------------   |
   |                                   |
   |-------- PROMPT:<b64> --------->  |  (t=5s, user sends prompt)
   |  <------- STATUS:<b64> --------   |  ("Connecting to LLM...")
   |  <------- CHUNK:<b64> ---------   |  (first token, ~1-2s)
   |  <------- CHUNK:<b64> ---------   |  (subsequent tokens)
   |       ... (N CHUNK messages) ...  |
   |  <------- DONE ----------------   |  (response complete)
   |                                   |
   |-------- PING ------------------>  |  (t=10s)
   |  <------- PONG ----------------   |
   |                                   |
   |-------- PING ------------------>  |  (t=20s)
   |                                   |  [no PONG within 15s]
   |  [mark UART disconnected]         |  [ESP32 offline]
```

---

## Hardware Command Flow: NFC Read

```
ESP32 (Agent)                        Flipper Zero
       |                                   |
       |--- HW:NFC:READ\n -------------->  |  Agent requests NFC read
       |                                   |  [Flipper activates NFC reader]
       |                                   |  [User taps card — up to 30s timeout]
       |  <-- HW:NFC:DATA:<base64>\n ----  |  Flipper sends raw tag bytes
       |                                   |
       |  [Agent decodes bytes, formats    |
       |   as hex, resumes tool result]    |
       |--- CHUNK:<b64> ----------------> |
       |--- DONE -----------------------> |
```

On timeout (no NFC data within 30 seconds), the ESP32 resumes the agent loop with a timeout error
result for the `flipper_nfc_read` tool.

---

## Heartbeat Protocol

- Flipper sends `PING\n` every **10 seconds**.
- ESP32 replies with `PONG\n` immediately upon receipt.
- Flipper tracks `last_pong_tick` using the Furi tick counter.
- If `(current_tick - last_pong_tick) > 15000ms`, the Flipper marks the ESP32 as disconnected and
  shows a hollow indicator dot in the UI header.
- On reconnection (next successful PONG), the indicator returns to filled.
- The ESP32 does not initiate heartbeats; it only responds.

---

## Constraints

| Parameter          | Value              |
|--------------------|--------------------|
| Max message length | 4096 bytes         |
| Baud rate          | 115200             |
| Framing            | ASCII, `\n`-delimited |
| Encoding           | RFC 4648 Base64    |
| Flipper TX pin     | GPIO 13 (USART1 TX)|
| Flipper RX pin     | GPIO 14 (USART1 RX)|
| ESP32-S3 TX pin    | GPIO 17            |
| ESP32-S3 RX pin    | GPIO 18            |
