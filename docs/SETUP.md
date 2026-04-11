# FlipperClaw Setup Guide

## Hardware Requirements

| Component              | Option A (Budget)              | Option B (Official)               |
|------------------------|-------------------------------|-----------------------------------|
| Flipper Zero           | Any revision                  | Any revision                      |
| ESP32-S3               | Generic ESP32-S3 dev board    | Flipper Zero WiFi Devboard (ESP32-S2, limited) |
| Wires                  | 4× female-to-female jumpers   | Included ribbon cable             |
| USB cable              | USB-C (ESP32 flashing)        | USB-C                             |

> **Recommended:** Generic ESP32-S3-DevKitC-1 (~$10) for full PSRAM support and dual-core operation.

---

## Hardware Wiring

If using a generic ESP32-S3 dev board (not the official Flipper WiFi Devboard):

| ESP32-S3 Pin   | Flipper GPIO Pin | Function           |
|----------------|------------------|--------------------|
| GND            | Pin 11 (GND)     | Common ground      |
| 3.3V           | Pin 9 (3.3V)     | Power ESP32 from Flipper (optional) |
| GPIO 17 (TX)   | Pin 14 (RX)      | Data: ESP32 → Flipper |
| GPIO 18 (RX)   | Pin 13 (TX)      | Data: Flipper → ESP32 |

> **Note:** Cross TX↔RX: ESP32 TX connects to Flipper RX, and vice versa.  
> **Note:** If powering ESP32 from USB independently, skip the 3.3V wire but keep GND common.

---

## ESP32 Setup

### 1. Install ESP-IDF v5.5

```bash
cd flipperclaw
./esp32/scripts/setup_idf.sh
```

This script clones ESP-IDF v5.5, runs the install script, and exports the environment. It will
take a few minutes on first run.

Alternatively, install manually:

```bash
git clone -b v5.5 --recursive https://github.com/espressif/esp-idf.git ~/esp/esp-idf
~/esp/esp-idf/install.sh esp32s3
. ~/esp/esp-idf/export.sh
```

### 2. Configure Secrets

```bash
cp esp32/main/fc_secrets.h.example esp32/main/fc_secrets.h
```

Edit `esp32/main/fc_secrets.h` and fill in:

```cpp
#define FC_SECRET_WIFI_SSID        "YourWiFiName"
#define FC_SECRET_WIFI_PASS        "YourWiFiPassword"
#define FC_SECRET_API_KEY          "sk-ant-api03-xxxxx"   // Anthropic API key
#define FC_SECRET_MODEL_PROVIDER   "anthropic"            // or "openai"
#define FC_SECRET_TAVILY_KEY       ""                     // optional, for web search
#define FC_SECRET_BRAVE_KEY        ""                     // fallback web search
```

> **Security:** `fc_secrets.h` is listed in `.gitignore`. Never commit it.

### 3. Build

```bash
cd esp32
./scripts/build.sh
```

This configures the build for the `esp32s3` target and runs `idf.py build`.

### 4. Flash

```bash
./scripts/flash.sh /dev/ttyUSB0
```

Replace `/dev/ttyUSB0` with your actual serial port:
- Linux: `/dev/ttyUSB0` or `/dev/ttyACM0`
- macOS: `/dev/cu.usbserial-*` or `/dev/cu.SLAB_USBtoUART`
- Windows: `COM3` (adjust as needed)

### 5. Configure via Serial CLI

Connect to the ESP32 over USB serial:

```bash
screen /dev/ttyUSB0 115200
# or
idf.py -p /dev/ttyUSB0 monitor
```

You will see the `fc> ` prompt. Verify your settings:

```
fc> config_show
```

All CLI settings are stored in NVS (non-volatile storage) and persist across reboots.

#### Available CLI Commands

| Command                              | Description                              |
|--------------------------------------|------------------------------------------|
| `wifi_set <ssid> <password>`         | Set WiFi credentials                     |
| `set_api_key <key>`                  | Set LLM API key                          |
| `set_model_provider <anthropic\|openai>` | Choose LLM provider                  |
| `set_model <model_name>`             | Set specific model (e.g. claude-haiku-4-5-20251001) |
| `set_tavily_key <key>`               | Set Tavily web search API key            |
| `set_brave_key <key>`                | Set Brave Search fallback key            |
| `memory_read [filename]`             | Read a file from SPIFFS                  |
| `memory_write "<content>"`           | Write content to MEMORY.md              |
| `config_show`                        | Display all current configuration        |
| `config_reset`                       | Reset all settings to defaults           |
| `status`                             | Show WiFi status, heap, UART state       |
| `restart`                            | Restart the ESP32                        |

---

## Flipper Setup

### 1. Install uFBT

```bash
pip3 install ufbt
```

Or use pipx for isolation:

```bash
pipx install ufbt
```

### 2. Build the FAP

```bash
cd flipper
ufbt
```

The compiled app will be at `flipper/dist/flipperclaw.fap`.

### 3. Install on Flipper

**Option A — qFlipper / direct copy:**
1. Connect Flipper Zero via USB.
2. Open qFlipper (or mount as USB drive).
3. Copy `dist/flipperclaw.fap` to `SD Card/apps/GPIO/flipperclaw.fap`.

**Option B — ufbt install:**
```bash
cd flipper
ufbt launch  # builds and launches directly over USB
```

### 4. Launch the App

On the Flipper Zero:
1. Navigate to: **Apps → GPIO → FlipperClaw**
2. The app starts and shows "Disconnected" until the ESP32 is powered and connected.

---

## First Boot Walkthrough

1. Power on ESP32 (USB or 3.3V from Flipper).
2. ESP32 boots, initialises SPIFFS, and begins WiFi connection.
3. Status message `"Connecting to WiFi..."` is sent to Flipper and displayed on screen.
4. On successful WiFi connect: `"WiFi connected"` status appears.
5. The ESP32 responds to the Flipper's PING with PONG — the status dot in the UI header fills.
6. Press **OK** on the Flipper to open the input view.
7. Use **UP/DOWN** to cycle characters, **LEFT/RIGHT** to move cursor, **OK** to send prompt.
8. Watch the streaming response appear on the 128×64 display.

---

## Troubleshooting

| Symptom                          | Likely Cause                        | Fix                                               |
|----------------------------------|-------------------------------------|---------------------------------------------------|
| Status dot is hollow (empty)     | No PONG from ESP32                  | Check wiring, check ESP32 power, check baud rate  |
| "NO_WIFI" error                  | WiFi credentials wrong/not set      | Run `fc> wifi_set <ssid> <pass>` via CLI          |
| "API_ERR" error                  | API key invalid or expired          | Run `fc> set_api_key <key>` via CLI               |
| "TIMEOUT" error                  | No internet or API unreachable      | Check WiFi, check firewall                        |
| FAP not visible on Flipper       | Wrong SD card path                  | Ensure file is at `apps/GPIO/flipperclaw.fap`     |
| Garbled characters on screen     | Base64 decode mismatch              | Verify both sides use RFC 4648 standard           |
| ESP32 not detected on serial     | Driver not installed                | Install CP210x or CH340 driver for your OS        |

---

## Security Notes

- `fc_secrets.h` is **gitignored** — never commit API keys.
- NVS keys are stored unencrypted in flash by default. For production, enable NVS encryption.
- The UART protocol has no authentication. Physical access to the wires = full control.
- API keys transit HTTPS to the LLM provider — TLS is enforced (no insecure fallback).
