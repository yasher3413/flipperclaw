/**
 * @file uart_bridge.c
 * @brief Furi HAL UART bridge — Base64 framing, IRQ byte capture, line parser.
 *
 * Architecture:
 *   IRQ callback → FuriStreamBuffer (minimal work in IRQ)
 *   uart_rx_thread_fn → drain stream buffer → accumulate line → parse_line()
 *   parse_line() → decode Base64 payload → post AppEvent to event_queue
 */

#include "uart_bridge.h"
#include <furi.h>
#include <furi_hal.h>
#include <gui/view_dispatcher.h>
#include <nfc/nfc.h>
#include <nfc/nfc_scanner.h>
#include <lib/subghz/subghz_setting.h>
#include <lib/subghz/subghz_file_encoder_worker.h>
#include <lib/subghz/devices/devices.h>
#include <lib/subghz/devices/cc1101_int/cc1101_int_interconnect.h>
#include <lib/flipper_format/flipper_format.h>
#include <lib/infrared/encoder_decoder/infrared.h>
#include <lib/infrared/worker/infrared_transmit.h>
#include <storage/storage.h>
#include <stdlib.h>
#include <string.h>

#define TAG "uart_bridge"
#define RX_STREAM_BUF_SIZE 4096U
#define RX_LINE_BUF_SIZE   4096U

// ---------------------------------------------------------------------------
// Base64 tables (RFC 4648)
// ---------------------------------------------------------------------------

static const char B64_ENC[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static const int8_t B64_DEC[256] = {
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
    52,53,54,55,56,57,58,59,60,61,-1,-1,-1, 0,-1,-1,
    -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
    15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
    -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
    41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
};

size_t fc_base64_encode(const uint8_t* src, size_t src_len,
                        char* dst, size_t dst_size) {
    size_t out_len = ((src_len + 2) / 3) * 4;
    if (dst_size < out_len + 1) return 0;

    size_t i, j = 0;
    for (i = 0; i < src_len; i += 3) {
        uint32_t b = (uint32_t)src[i] << 16;
        if (i + 1 < src_len) b |= (uint32_t)src[i + 1] << 8;
        if (i + 2 < src_len) b |= (uint32_t)src[i + 2];

        dst[j++] = B64_ENC[(b >> 18) & 0x3F];
        dst[j++] = B64_ENC[(b >> 12) & 0x3F];
        dst[j++] = (i + 1 < src_len) ? B64_ENC[(b >> 6) & 0x3F] : '=';
        dst[j++] = (i + 2 < src_len) ? B64_ENC[b & 0x3F] : '=';
    }
    dst[j] = '\0';
    return j;
}

size_t fc_base64_decode(const char* src, size_t src_len,
                        uint8_t* dst, size_t dst_size) {
    if (src_len % 4 != 0) return 0;
    size_t max_out = (src_len / 4) * 3;
    if (dst_size < max_out) return 0;

    size_t out = 0;
    for (size_t i = 0; i < src_len; i += 4) {
        int8_t a = B64_DEC[(uint8_t)src[i]];
        int8_t b = B64_DEC[(uint8_t)src[i + 1]];
        int8_t c = B64_DEC[(uint8_t)src[i + 2]];
        int8_t d = B64_DEC[(uint8_t)src[i + 3]];
        if (a < 0 || b < 0) return 0;

        uint32_t triple = ((uint32_t)a << 18) | ((uint32_t)b << 12) |
                          ((uint32_t)(c < 0 ? 0 : c) << 6) |
                          (uint32_t)(d < 0 ? 0 : d);

        dst[out++] = (triple >> 16) & 0xFF;
        if (src[i + 2] != '=') dst[out++] = (triple >> 8) & 0xFF;
        if (src[i + 3] != '=') dst[out++] = triple & 0xFF;
    }
    return out;
}

// ---------------------------------------------------------------------------
// NFC scan helpers
// ---------------------------------------------------------------------------

typedef struct {
    FuriSemaphore* sem;
    NfcProtocol    protocol;
    bool           detected;
} NfcScanCtx;

static void nfc_scan_callback(NfcScannerEvent event, void* context) {
    NfcScanCtx* ctx = context;
    if(event.type == NfcScannerEventTypeDetected && event.data.protocol_num > 0) {
        ctx->protocol = event.data.protocols[0];
        ctx->detected = true;
        furi_semaphore_release(ctx->sem);
    }
}

// Map NfcProtocol enum values to human-readable strings.
// Order matches the NfcProtocol enum in <nfc/protocols/nfc_protocol.h>.
static const char* nfc_protocol_name(NfcProtocol proto) {
    switch((int)proto) {
    case 0:  return "ISO14443-3A";
    case 1:  return "ISO14443-3B";
    case 2:  return "ISO14443-4A";
    case 3:  return "ISO14443-4B";
    case 4:  return "ISO15693";
    case 5:  return "FeliCa";
    case 6:  return "MIFARE Classic";
    case 7:  return "MIFARE Ultralight";
    case 8:  return "MIFARE DESFire";
    case 9:  return "EMV";
    default: return "Unknown";
    }
}

// ---------------------------------------------------------------------------
// Sub-GHz replay helpers
// ---------------------------------------------------------------------------

static void subghz_tx_done_cb(void* context) {
    furi_semaphore_release((FuriSemaphore*)context);
}

// Map preset name (as stored in .sub files) to FuriHalSubGhzPreset enum.
static FuriHalSubGhzPreset subghz_preset_from_name(const char* name) {
    if(strcmp(name, "AM650") == 0)      return FuriHalSubGhzPresetOok650Async;
    if(strcmp(name, "AM270") == 0)      return FuriHalSubGhzPresetOok270Async;
    if(strcmp(name, "FM238") == 0)      return FuriHalSubGhzPreset2FSKDev238Async;
    if(strcmp(name, "FM476") == 0)      return FuriHalSubGhzPreset2FSKDev476Async;
    if(strcmp(name, "MSK99_97Kb") == 0) return FuriHalSubGhzPresetMSK99_97KbAsync;
    if(strcmp(name, "GFSK9_99Kb") == 0) return FuriHalSubGhzPresetGFSK9_99KbAsync;
    return FuriHalSubGhzPresetCustom;
}

static void uart_do_subghz_replay(FlipperClawApp* app, const char* filename) {
    // Build full SD card path
    char full_path[256];
    if(filename[0] == '/') {
        snprintf(full_path, sizeof(full_path), "%s", filename);
    } else {
        snprintf(full_path, sizeof(full_path), "/ext/subghz/%s", filename);
    }
    FURI_LOG_I(TAG, "Sub-GHz replay: %s", full_path);

    // --- Step 1: read Frequency and Preset from the .sub file ---
    uint32_t frequency = 433920000;
    char preset_name[64] = "AM650";

    Storage* storage = furi_record_open(RECORD_STORAGE);
    FlipperFormat* ff = flipper_format_file_alloc(storage);
    if(!flipper_format_file_open_existing(ff, full_path)) {
        FURI_LOG_W(TAG, "Sub-GHz: cannot open %s", full_path);
        flipper_format_free(ff);
        furi_record_close(RECORD_STORAGE);
        return;
    }
    flipper_format_read_uint32(ff, "Frequency", &frequency, 1);
    FuriString* preset_str = furi_string_alloc();
    if(flipper_format_read_string(ff, "Preset", preset_str)) {
        snprintf(preset_name, sizeof(preset_name), "%s", furi_string_get_cstr(preset_str));
    }
    furi_string_free(preset_str);

    // --- Step 2: resolve preset enum + data (handles custom presets too) ---
    FuriHalSubGhzPreset preset_enum = subghz_preset_from_name(preset_name);
    uint8_t* preset_data = NULL;

    SubGhzSetting* setting = subghz_setting_alloc();
    subghz_setting_load(setting, EXT_PATH("subghz/assets/setting_user"));

    if(preset_enum == FuriHalSubGhzPresetCustom) {
        // The custom registers live in the .sub file under "Custom_preset_data"
        // Rewind and load them into the setting registry
        flipper_format_rewind(ff);
        subghz_setting_load_custom_preset(setting, preset_name, ff);
        preset_data = subghz_setting_get_preset_data_by_name(setting, preset_name);
    }

    flipper_format_file_close(ff);
    flipper_format_free(ff);
    furi_record_close(RECORD_STORAGE);

    // --- Step 3: transmit ---
    subghz_devices_init();
    const SubGhzDevice* device = subghz_devices_get_by_name(SUBGHZ_DEVICE_CC1101_INT_NAME);

    FuriSemaphore* done_sem = furi_semaphore_alloc(1, 0);
    SubGhzFileEncoderWorker* encoder = subghz_file_encoder_worker_alloc();
    subghz_file_encoder_worker_callback_end(encoder, subghz_tx_done_cb, done_sem);

    if(subghz_devices_is_frequency_valid(device, frequency) &&
       subghz_file_encoder_worker_start(encoder, full_path, SUBGHZ_DEVICE_CC1101_INT_NAME)) {

        furi_delay_ms(200); // let worker initialise

        subghz_devices_begin(device);
        subghz_devices_load_preset(device, preset_enum, preset_data);
        subghz_devices_set_frequency(device, frequency);
        subghz_devices_set_tx(device);
        subghz_devices_start_async_tx(
            device, subghz_file_encoder_worker_get_level_duration, encoder);

        furi_semaphore_acquire(done_sem, 30000);

        subghz_devices_stop_async_tx(device);
        subghz_devices_idle(device);
        subghz_devices_end(device);
        subghz_file_encoder_worker_stop(encoder);
    } else {
        FURI_LOG_W(TAG, "Sub-GHz: invalid freq %lu or file open failed", (unsigned long)frequency);
    }

    subghz_file_encoder_worker_free(encoder);
    subghz_devices_deinit();
    furi_semaphore_free(done_sem);
    subghz_setting_free(setting);

    // Notify ESP32 (base64 of "Sub-GHz done")
    const uint8_t status[] = "STATUS:U3ViLUdIeiBkb25l\n";
    furi_hal_serial_tx(app->serial_handle, status, sizeof(status) - 1);
}

// Map .ir file protocol name to InfraredProtocol enum
static InfraredProtocol ir_protocol_from_name(const char* name) {
    if(strcmp(name, "NEC") == 0)       return InfraredProtocolNEC;
    if(strcmp(name, "NECext") == 0)    return InfraredProtocolNECext;
    if(strcmp(name, "NEC42") == 0)     return InfraredProtocolNEC42;
    if(strcmp(name, "NEC42ext") == 0)  return InfraredProtocolNEC42ext;
    if(strcmp(name, "Samsung32") == 0) return InfraredProtocolSamsung32;
    if(strcmp(name, "RC6") == 0)       return InfraredProtocolRC6;
    if(strcmp(name, "RC5") == 0)       return InfraredProtocolRC5;
    if(strcmp(name, "RC5X") == 0)      return InfraredProtocolRC5X;
    if(strcmp(name, "SIRC") == 0)      return InfraredProtocolSIRC;
    if(strcmp(name, "SIRC15") == 0)    return InfraredProtocolSIRC15;
    if(strcmp(name, "SIRC20") == 0)    return InfraredProtocolSIRC20;
    if(strcmp(name, "Kaseikyo") == 0)  return InfraredProtocolKaseikyo;
    if(strcmp(name, "RCA") == 0)       return InfraredProtocolRCA;
    if(strcmp(name, "Pioneer") == 0)   return InfraredProtocolPioneer;
    return InfraredProtocolUnknown;
}

static void uart_do_ir_send(FlipperClawApp* app, const char* filename) {
    // Build full SD card path
    char full_path[256];
    if(filename[0] == '/') {
        snprintf(full_path, sizeof(full_path), "%s", filename);
    } else {
        snprintf(full_path, sizeof(full_path), "/ext/infrared/%s", filename);
    }
    FURI_LOG_I(TAG, "IR send: %s", full_path);

    Storage* storage = furi_record_open(RECORD_STORAGE);
    FlipperFormat* ff = flipper_format_file_alloc(storage);

    if(!flipper_format_file_open_existing(ff, full_path)) {
        FURI_LOG_W(TAG, "IR: cannot open %s", full_path);
        flipper_format_free(ff);
        furi_record_close(RECORD_STORAGE);
        const char* err = "ERROR:IR_FILE_NOT_FOUND\n";
        furi_hal_serial_tx(app->serial_handle, (uint8_t*)err, strlen(err));
        return;
    }

    bool ok = false;
    FuriString* str = furi_string_alloc();

    // Read first signal name (sequentially from file start)
    if(!flipper_format_read_string(ff, "name", str)) {
        FURI_LOG_W(TAG, "IR: no 'name' field in %s", full_path);
        goto cleanup;
    }
    FURI_LOG_I(TAG, "IR: transmitting signal '%s'", furi_string_get_cstr(str));

    // Read signal type ("parsed" or "raw")
    if(!flipper_format_read_string(ff, "type", str)) {
        FURI_LOG_W(TAG, "IR: no 'type' field");
        goto cleanup;
    }

    if(furi_string_equal_str(str, "parsed")) {
        // Read protocol name
        FuriString* proto_str = furi_string_alloc();
        flipper_format_read_string(ff, "protocol", proto_str);
        InfraredProtocol proto = ir_protocol_from_name(furi_string_get_cstr(proto_str));
        furi_string_free(proto_str);

        // address and command stored as hex bytes, little-endian
        uint8_t addr_bytes[4] = {0};
        uint8_t cmd_bytes[4]  = {0};
        flipper_format_read_hex(ff, "address", addr_bytes, 4);
        flipper_format_read_hex(ff, "command",  cmd_bytes, 4);

        uint32_t address = (uint32_t)addr_bytes[0]        |
                           ((uint32_t)addr_bytes[1] << 8)  |
                           ((uint32_t)addr_bytes[2] << 16) |
                           ((uint32_t)addr_bytes[3] << 24);
        uint32_t command = (uint32_t)cmd_bytes[0]         |
                           ((uint32_t)cmd_bytes[1] << 8)   |
                           ((uint32_t)cmd_bytes[2] << 16)  |
                           ((uint32_t)cmd_bytes[3] << 24);

        InfraredMessage msg = {
            .protocol = proto,
            .address  = address,
            .command  = command,
            .repeat   = false,
        };
        infrared_send(&msg, 1);
        ok = true;

    } else if(furi_string_equal_str(str, "raw")) {
        uint32_t frequency  = INFRARED_COMMON_CARRIER_FREQUENCY;
        float    duty_cycle = INFRARED_COMMON_DUTY_CYCLE;
        flipper_format_read_uint32(ff, "frequency",  &frequency,   1);
        flipper_format_read_float (ff, "duty_cycle", &duty_cycle,  1);

        uint32_t count = 0;
        if(flipper_format_get_value_count(ff, "data", &count) && count > 0) {
            uint32_t* timings = malloc(count * sizeof(uint32_t));
            if(timings) {
                if(flipper_format_read_uint32(ff, "data", timings, count)) {
                    infrared_send_raw_ext(timings, count, true, frequency, duty_cycle);
                    ok = true;
                }
                free(timings);
            }
        }
    }

cleanup:
    furi_string_free(str);
    flipper_format_file_close(ff);
    flipper_format_free(ff);
    furi_record_close(RECORD_STORAGE);

    if(ok) {
        // base64("IR done") = "SVIgZG9uZQ=="
        const uint8_t status[] = "STATUS:SVIgZG9uZQ==\n";
        furi_hal_serial_tx(app->serial_handle, status, sizeof(status) - 1);
    } else {
        const char* err = "ERROR:IR_SEND_FAILED\n";
        furi_hal_serial_tx(app->serial_handle, (uint8_t*)err, strlen(err));
    }
}

static void uart_send_nfc_data(FlipperClawApp* app, const char* json) {
    size_t json_len  = strlen(json);
    size_t b64_size  = ((json_len + 2) / 3) * 4 + 1;
    char*  b64       = malloc(b64_size);
    if(!b64) return;
    fc_base64_encode((const uint8_t*)json, json_len, b64, b64_size);

    size_t msg_len = 12 + strlen(b64) + 2; // "HW:NFC:DATA:" + b64 + "\n\0"
    char*  msg     = malloc(msg_len);
    if(msg) {
        snprintf(msg, msg_len, "HW:NFC:DATA:%s\n", b64);
        furi_hal_serial_tx(app->serial_handle, (uint8_t*)msg, strlen(msg));
        free(msg);
    }
    free(b64);
}

// ---------------------------------------------------------------------------
// RX thread state (stored in heap, pointed to by app->uart_rx_thread userdata)
// ---------------------------------------------------------------------------

typedef struct {
    FlipperClawApp* app;
    FuriStreamBuffer* stream;    // IRQ → thread byte pipe
    char line_buf[RX_LINE_BUF_SIZE];
    size_t line_len;
} UartRxCtx;

static UartRxCtx* g_rx_ctx = NULL;   // single instance; needed by IRQ callback

// ---------------------------------------------------------------------------
// IRQ callback — called from UART IRQ, must be minimal
// ---------------------------------------------------------------------------

static void uart_irq_cb(FuriHalSerialHandle* handle, FuriHalSerialRxEvent event, void* ctx) {
    UNUSED(ctx);
    if(event == FuriHalSerialRxEventData) {
        uint8_t data = furi_hal_serial_async_rx(handle);
        FuriStreamBuffer* stream = g_rx_ctx ? g_rx_ctx->stream : NULL;
        if(stream) {
            furi_stream_buffer_send(stream, &data, 1, 0);
        }
    }
}

// ---------------------------------------------------------------------------
// Line parser — called from RX thread, posts AppEvents
// ---------------------------------------------------------------------------

static void uart_parse_line(FlipperClawApp* app, const char* buf, size_t len) {
    if(len == 0) return;

    // Decode helpers — static to avoid blowing the thread stack
    static uint8_t decoded_buf[2048];
    static char    decoded_str[2048];

    // Strip trailing \r
    if(len > 0 && buf[len - 1] == '\r') len--;

    AppEvent evt = {0};

    // HW: prefix messages have multiple colons — handle before generic colon search
    if(len >= 17 && strncmp(buf, "HW:SUBGHZ:REPLAY:", 17) == 0) {
        (void)decoded_buf;
        (void)decoded_str;
        uart_do_subghz_replay(app, buf + 17);
        return;
    }

    if(len >= 12 && strncmp(buf, "HW:IR:SEND:", 11) == 0) {
        (void)decoded_buf;
        (void)decoded_str;
        uart_do_ir_send(app, buf + 11);
        return;
    }

    if(len >= 11 && strncmp(buf, "HW:NFC:READ", 11) == 0) {
        FURI_LOG_I(TAG, "HW:NFC:READ — scanning...");
        (void)decoded_buf;
        (void)decoded_str;

        NfcScanCtx scan_ctx = {
            .sem      = furi_semaphore_alloc(1, 0),
            .detected = false,
            .protocol = 0,
        };

        Nfc*        nfc     = nfc_alloc();
        NfcScanner* scanner = nfc_scanner_alloc(nfc);
        nfc_scanner_start(scanner, nfc_scan_callback, &scan_ctx);

        // Block up to 30 s for a tap
        FuriStatus status = furi_semaphore_acquire(scan_ctx.sem, 30000);

        nfc_scanner_stop(scanner);
        nfc_scanner_free(scanner);
        nfc_free(nfc);
        furi_semaphore_free(scan_ctx.sem);

        if(status == FuriStatusOk && scan_ctx.detected) {
            char json[128];
            snprintf(json, sizeof(json),
                     "{\"protocol\":\"%s\"}",
                     nfc_protocol_name(scan_ctx.protocol));
            uart_send_nfc_data(app, json);
        } else {
            const char* err = "ERROR:TIMEOUT\n";
            furi_hal_serial_tx(app->serial_handle, (uint8_t*)err, strlen(err));
        }
        return;
    }

// Wake the ViewDispatcher after posting any event so drain_event_queue() runs promptly.
#define POST_AND_WAKE(app, evt_ptr) \
    do { \
        furi_message_queue_put((app)->event_queue, (evt_ptr), 0); \
        view_dispatcher_send_custom_event((app)->view_dispatcher, 0); \
    } while(0)

    // PONG
    if(len == 4 && strncmp(buf, "PONG", 4) == 0) {
        evt.type = AppEventTypeUartPong;
        POST_AND_WAKE(app, &evt);
        return;
    }

    // DONE
    if(len == 4 && strncmp(buf, "DONE", 4) == 0) {
        evt.type = AppEventTypeUartDone;
        POST_AND_WAKE(app, &evt);
        return;
    }

    // Find colon separator
    const char* colon = NULL;
    for(size_t i = 0; i < len; i++) {
        if(buf[i] == ':') { colon = buf + i; break; }
    }

    if(!colon) {
        FURI_LOG_W(TAG, "No colon in line, ignoring");
        return;
    }

    size_t type_len      = (size_t)(colon - buf);
    const char* payload  = colon + 1;
    size_t payload_len   = len - type_len - 1;

    // CHUNK:<b64>
    if(type_len == 5 && strncmp(buf, "CHUNK", 5) == 0) {
        size_t dec_len = fc_base64_decode(payload, payload_len,
                                           decoded_buf, sizeof(decoded_buf) - 1);
        if(dec_len == 0 && payload_len > 0) {
            FURI_LOG_W(TAG, "CHUNK base64 decode failed");
            return;
        }
        decoded_buf[dec_len] = '\0';

        char* text = malloc(dec_len + 1);
        if(!text) return;
        memcpy(text, decoded_buf, dec_len + 1);

        evt.type            = AppEventTypeUartChunk;
        evt.data.chunk.text = text;   // receiver must free
        POST_AND_WAKE(app, &evt);
        return;
    }

    // ERROR:<code>
    if(type_len == 5 && strncmp(buf, "ERROR", 5) == 0) {
        evt.type = AppEventTypeUartError;
        size_t copy_len = payload_len < sizeof(evt.data.error.code) - 1
                          ? payload_len
                          : sizeof(evt.data.error.code) - 1;
        memcpy(evt.data.error.code, payload, copy_len);
        evt.data.error.code[copy_len] = '\0';
        POST_AND_WAKE(app, &evt);
        return;
    }

    // STATUS:<b64>
    if(type_len == 6 && strncmp(buf, "STATUS", 6) == 0) {
        size_t dec_len = fc_base64_decode(payload, payload_len,
                                           decoded_buf, sizeof(decoded_buf) - 1);
        decoded_buf[dec_len] = '\0';
        evt.type = AppEventTypeUartStatus;
        size_t copy_len = dec_len < sizeof(evt.data.status.text) - 1
                          ? dec_len
                          : sizeof(evt.data.status.text) - 1;
        memcpy(evt.data.status.text, decoded_buf, copy_len);
        evt.data.status.text[copy_len] = '\0';
        POST_AND_WAKE(app, &evt);
        return;
    }

    (void)decoded_str;

    FURI_LOG_D(TAG, "Unknown message type (len=%zu)", type_len);
}

// ---------------------------------------------------------------------------
// RX thread function
// ---------------------------------------------------------------------------

static int32_t uart_rx_thread_fn(void* ctx) {
    UartRxCtx* rx = (UartRxCtx*)ctx;
    uint8_t byte;

    while(true) {
        // Block until a byte arrives (or thread is stopped via furi_thread_flags)
        size_t received = furi_stream_buffer_receive(rx->stream, &byte, 1,
                                                      FuriWaitForever);
        if(received == 0) continue;

        if(byte == '\n') {
            if(rx->line_len > 0) {
                rx->line_buf[rx->line_len] = '\0';
                uart_parse_line(rx->app, rx->line_buf, rx->line_len);
                rx->line_len = 0;
            }
        } else {
            if(rx->line_len < RX_LINE_BUF_SIZE - 1) {
                rx->line_buf[rx->line_len++] = (char)byte;
            } else {
                FURI_LOG_W(TAG, "RX line overflow, discarding");
                rx->line_len = 0;
            }
        }

        // Check if thread should stop
        uint32_t flags = furi_thread_flags_get();
        if(flags & 0x1) break;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Init / deinit
// ---------------------------------------------------------------------------

void uart_bridge_init(FlipperClawApp* app) {
    furi_assert(app);

    UartRxCtx* ctx = malloc(sizeof(UartRxCtx));
    furi_assert(ctx);
    memset(ctx, 0, sizeof(UartRxCtx));
    ctx->app    = app;
    ctx->stream = furi_stream_buffer_alloc(RX_STREAM_BUF_SIZE, 1);
    furi_assert(ctx->stream);

    g_rx_ctx = ctx;

    // Acquire and configure serial port (USART1: TX=13, RX=14)
    app->serial_handle = furi_hal_serial_control_acquire(FuriHalSerialIdUsart);
    furi_assert(app->serial_handle);
    furi_hal_serial_init(app->serial_handle, FC_UART_BAUD);
    furi_hal_serial_async_rx_start(app->serial_handle, uart_irq_cb, ctx, false);

    // Start RX thread
    app->uart_rx_thread = furi_thread_alloc_ex(
        "uart_rx", 6144, uart_rx_thread_fn, ctx);
    furi_thread_start(app->uart_rx_thread);

    FURI_LOG_I(TAG, "UART bridge initialised (baud=%u)", FC_UART_BAUD);
}

void uart_bridge_deinit(FlipperClawApp* app) {
    furi_assert(app);

    // Signal RX thread to stop
    if(app->uart_rx_thread) {
        furi_thread_flags_set(furi_thread_get_id(app->uart_rx_thread), 0x1);
        furi_thread_join(app->uart_rx_thread);
        furi_thread_free(app->uart_rx_thread);
        app->uart_rx_thread = NULL;
    }

    if(app->serial_handle) {
        furi_hal_serial_async_rx_stop(app->serial_handle);
        furi_hal_serial_deinit(app->serial_handle);
        furi_hal_serial_control_release(app->serial_handle);
        app->serial_handle = NULL;
    }

    if(g_rx_ctx) {
        furi_stream_buffer_free(g_rx_ctx->stream);
        free(g_rx_ctx);
        g_rx_ctx = NULL;
    }

    FURI_LOG_I(TAG, "UART bridge deinitialised");
}

// ---------------------------------------------------------------------------
// Send helpers
// ---------------------------------------------------------------------------

void uart_send_prompt(FlipperClawApp* app, const char* text) {
    furi_assert(app);
    furi_assert(text);

    size_t text_len = strlen(text);
    size_t b64_size = base64_encoded_len(text_len);
    char* b64 = malloc(b64_size);
    if(!b64) return;

    fc_base64_encode((const uint8_t*)text, text_len, b64, b64_size);

    // Format: "PROMPT:<b64>\n"
    size_t msg_len = 7 + strlen(b64) + 2; // "PROMPT:" + b64 + "\n\0"
    char* msg = malloc(msg_len);
    if(!msg) { free(b64); return; }
    snprintf(msg, msg_len, "PROMPT:%s\n", b64);

    furi_hal_serial_tx(app->serial_handle, (uint8_t*)msg, strlen(msg));

    free(b64);
    free(msg);
}

void uart_send_ping(FlipperClawApp* app) {
    furi_assert(app);
    const char* ping = "PING\n";
    furi_hal_serial_tx(app->serial_handle, (uint8_t*)ping, strlen(ping));
}

void uart_send_cancel(FlipperClawApp* app) {
    furi_assert(app);
    const char* cancel = "CANCEL\n";
    furi_hal_serial_tx(app->serial_handle, (uint8_t*)cancel, strlen(cancel));
}
