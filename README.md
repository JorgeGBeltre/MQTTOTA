# MQTTOTA — OTA Updates via MQTT/MQTTS

[![Arduino Library](https://img.shields.io/badge/Arduino-Library-teal)](https://www.arduinolibraries.info/libraries/mqttota)
[![Platform](https://img.shields.io/badge/platform-ESP32-blue)](https://www.espressif.com/en/products/socs/esp32)
[![MQTT 3.1.1](https://img.shields.io/badge/MQTT-3.1.1-orange)](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html)
[![License](https://img.shields.io/badge/license-MIT-green)](LICENSE)
[![Ask DeepWiki](https://deepwiki.com/badge.svg)](https://deepwiki.com/JorgeGBeltre/MQTTOTA)

---
**MQTTOTA** is an SDK that revolutionizes firmware management for ESP32-based IoT devices. By leveraging the power of MQTT/MQTTS protocols, it provides a seamless, secure, and scalable solution for Over-The-Air updates in distributed IoT ecosystems. Whether you're managing a handful of devices or thousands across global deployments, MQTTOTA ensures reliable firmware delivery with enterprise-level security and robust error handling.

## Table of Contents

- [What's New in v1.2.0](#whats-new-in-v110)
- [Overview](#overview)
- [Key Features](#key-features)
- [Installation](#installation)
- [Dependencies](#dependencies)
- [Basic Configuration](#basic-configuration)
- [Security — HMAC-SHA256 (v1.2.0)](#security--hmac-sha256-v110)
- [Standard MQTT Configuration](#standard-mqtt-configuration)
- [MQTTS (Secure) Configuration](#mqtts-secure-configuration)
- [Message Formats](#message-formats)
- [Advanced Configuration](#advanced-configuration)
- [Diagnostics and Troubleshooting](#diagnostics-and-troubleshooting)
- [Complete API](#complete-api)
- [Performance Considerations](#performance-considerations)
- [Best Practices](#best-practices)
- [Complete Workflows](#complete-workflows)
- [Backend Implementation](#backend-implementation)
- [Broker Compatibility](#broker-compatibility)
- [License](#license)
- [Contact](#contact)

---

## What's New in v1.2.0

This release resolves **8 issues** identified in a technical audit of v1.1.0. All changes are backward-compatible unless noted.

###  Security fixes

| Issue | Description | Solution |
|---|---|---|
| **#1** | `_calculateSHA256()` always returned `""` — SHA-256 was declared but never computed | Implemented with `mbedtls_sha256` — real incremental digest |
| **#2** | `verifyFirmwareSignature()` always returned `true` — HMAC was never verified | Real HMAC-SHA256 via `mbedtls_md_hmac`. New API: `setSecurityKey()` / `requireSignature()` |

###  Memory fixes

| Issue | Description | Solution |
|---|---|---|
| **#3** | `DynamicJsonDocument(32768)` allocated 32 KB on the heap on every message | All JSON documents migrated to `StaticJsonDocument` on the stack |
| **#6** | `getFreeOTASpace()` was commented out with a compilation error note | Implemented correctly using `esp_ota_get_next_update_partition()->size` |

###  Robustness fixes

| Issue | Description | Solution |
|---|---|---|
| **#4** | `delay(2000)` / `delay(3000)` before `ESP.restart()` blocked the FreeRTOS scheduler | Replaced with a non-blocking timer polled in `handle()`. **`handle()` must be called every `loop()`** |
| **#5** | Two parallel state variables: `_otaInProgress` and `_otaContext.inProgress` could go out of sync | Unified into `_otaContext.inProgress`. `isUpdateInProgress()` now reads a single source |
| **#7** | `_setState()` was only called in a few places — `getCurrentState()` returned stale values | `_setState()` is now called at every transition: `IDLE → RECEIVING → DECODING → VALIDATING → WRITING → COMPLETING → SUCCESS/ERROR` |
| **#8** | `esp_ota_mark_app_valid_cancel_rollback()` was never called automatically | Called in `begin()` whenever the running partition is an OTA partition |

###  Breaking behavior change (v1.2.0)

> In v1.0.1 the device restarted automatically via a blocking `delay()+ESP.restart()` after a successful OTA.
>
> In v1.2.0 the restart is **non-blocking** and is driven by a timer polled in `handle()`. If you do not call `handle()` in your `loop()`, the device will **not** restart automatically after OTA.
>
> **Action required:** Ensure `ota.handle()` is called every `loop()` iteration.

```cpp
void loop() {
    mqttClient.loop();
    ota.handle();   // Required: drives restart timer + timeout watchdog
}
```

---

## Overview

MQTTOTA is a robust SDK designed for ESP32 IoT deployments. It supports both single-message and chunked firmware transfers, integrates directly with the ESP-IDF OTA partition API (`esp_ota_ops.h`), and since v1.2.0 provides real cryptographic verification with SHA-256 and HMAC-SHA256 via mbedtls (bundled with ESP32 Arduino).

---

## Key Features

### Update Methods
- **Full OTA** — Single MQTT message with complete firmware
- **Chunked OTA** — Fragmented transfer for large firmware files with strict sequence validation
- **Native ESP-IDF OTA** — Uses `esp_ota_begin/write/end` directly; no dependency on `Update.h` layer

### Security (v1.2.0)
- **MQTTS** — Encrypted transport via TLS (delegated to the external MQTT client)
- **Real SHA-256** — Integrity digest computed with `mbedtls_sha256` on every chunk
- **Real HMAC-SHA256** — Firmware origin authentication via `setSecurityKey()` + `requireSignature()`
- **Image header verification** — Validates ESP32 magic number and segment count before writing
- **Configurable timeout** — 7-minute watchdog cancels hung updates

### Monitoring and Control
- **9-state machine** — `IDLE → RECEIVING → DECODING → VALIDATING → WRITING → COMPLETING → SUCCESS/ERROR/ABORTED`
- **Event callbacks** — `onProgress`, `onError`, `onSuccess`, `onStateChange`
- **OTA statistics** — bytes, chunk count, error count, average speed (KB/s)
- **Non-blocking restart** (v1.2.0) — restart scheduled after OTA without blocking `loop()`

---

## Installation

### Method 1: Manual
1. Download `MQTTOTA.h` and `MQTTOTA.cpp`
2. Create `MQTTOTA/` folder in `Arduino/libraries/`
3. Copy files there and restart Arduino IDE

### Method 2: PlatformIO
```ini
lib_deps =
    https://github.com/JorgeGBeltre/MQTTOTA.git
```

---

## Dependencies

```cpp
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>        // v6.19+ (ArduinoJson v7 not yet tested)
#include <Update.h>
#include <mbedtls/sha256.h>     // v1.2.0 — bundled with ESP32 Arduino, no extra install
#include <mbedtls/md.h>         // v1.2.0 — bundled with ESP32 Arduino, no extra install
#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include "esp_partition.h"
```

> No additional library installs are needed for mbedtls — it ships with the ESP32 Arduino core.

### Configuration macros (override before `#include "MQTTOTA.h"`)

```cpp
#define MQTT_OTA_JSON_SIZE       8192   // Stack size for JSON parser (v1.2.0: was 32768 heap)
#define MQTT_OTA_BUFFSIZE        1024   // Write buffer per chunk
#define MQTT_OTA_TIMEOUT_MS    420000  // Total OTA timeout (7 min)
#define MQTT_OTA_MAX_CHUNK_SIZE 65536  // Max decoded bytes per MQTT chunk
#define MQTT_OTA_MIN_MEMORY     40000  // Min free heap before starting OTA
#define MQTT_OTA_MAX_RETRIES        3  // Chunk retry limit
#define MQTT_OTA_HMAC_KEY_SIZE     64  // Max HMAC key length (bytes)
#define MQTT_OTA_RESTART_DELAY_MS 3000 // Non-blocking restart delay (ms)
```

> **Stack note:** `StaticJsonDocument<MQTT_OTA_JSON_SIZE>` lives on the stack of `loop()`. The default 8 KB fits comfortably in the ESP32's 8 KB `loop()` stack. If you use FreeRTOS tasks with custom stacks, ensure the task stack is ≥ 10 KB.

---

## Basic Configuration

```cpp
#include "MQTTOTA.h"

MQTTOTA ota;

void setup() {
    Serial.begin(115200);
    ota.begin("MyDevice", "1.0.0");
    // begin() also calls esp_ota_mark_app_valid_cancel_rollback()
    // automatically when running from an OTA partition (v1.2.0)
}

void loop() {
    ota.handle();   // Required every loop — drives restart timer and timeout
}
```

---

## Security — HMAC-SHA256 (v1.2.0)

v1.2.0 adds real firmware authentication. Without a configured key the behavior is identical to v1.0.1 (backward compatible).

### Enabling HMAC verification

```cpp
void setup() {
    ota.begin("MyDevice", "1.0.0");

    // Set shared secret (must match the key used by your backend to sign firmware)
    ota.setSecurityKey("my-super-secret-key-at-least-32-chars");

    // Optional: reject any OTA message that does not carry a valid HMAC
    ota.requireSignature(true);
}
```

### Backend: signing the firmware

Your backend must compute `HMAC-SHA256(decoded_firmware_bytes, key)` and send it as a hex string in the `checksum` field (or in a User Property if using MQTTOTAv5).

```python
# Python example
import hmac, hashlib

key   = b"my-super-secret-key-at-least-32-chars"
data  = open("firmware.bin", "rb").read()
sig   = hmac.new(key, data, hashlib.sha256).hexdigest()
print(sig)   # 64-char hex string → include in OTA message
```

### Manual verification in your code

```cpp
// Legacy overload — backward compat, does not compute HMAC without raw data
bool verifyFirmwareSignature(const String& signature);
```

---

## Standard MQTT Configuration

### Example with PubSubClient

```cpp
#include "MQTTOTA.h"
#include <PubSubClient.h>
#include <WiFi.h>

WiFiClient   wifiClient;
PubSubClient mqttClient(wifiClient);
MQTTOTA      ota;

const char* ssid      = "your_SSID";
const char* password  = "your_PASSWORD";
const char* mqttServer= "broker.hivemq.com";
const int   mqttPort  = 1883;
const char* otaTopic  = "devices/my_device/ota";

void mqttCallback(char* topic, byte* payload, unsigned int length) {
    String message;
    for (unsigned int i = 0; i < length; i++) message += (char)payload[i];
    ota.processMessage(String(topic), message);
}

void setup() {
    Serial.begin(115200);
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) { delay(500); }

    mqttClient.setServer(mqttServer, mqttPort);
    mqttClient.setCallback(mqttCallback);
    while (!mqttClient.connect("my_device")) { delay(5000); }
    mqttClient.subscribe(otaTopic);

    ota.begin("MyDevice", "1.0.0");
    ota.setMQTTConfig(
        [](const char* topic, const String& msg) { mqttClient.publish(topic, msg.c_str()); },
        []() { return mqttClient.connected(); },
        otaTopic
    );

    ota.onProgress([](int pct, const String& ver) {
        Serial.printf("OTA %d%% — v%s\n", pct, ver.c_str());
    });
    ota.onError([](const String& err, const String& ver) {
        Serial.printf("OTA error: %s (v%s)\n", err.c_str(), ver.c_str());
    });
    ota.onSuccess([](const String& ver) {
        Serial.printf("OTA complete: v%s\n", ver.c_str());
    });
}

void loop() {
    if (!mqttClient.connected()) { /* reconnect */ }
    mqttClient.loop();
    ota.handle();   // Required: non-blocking restart + timeout watchdog
}
```

---

## MQTTS (Secure) Configuration

```cpp
#include "MQTTOTA.h"
#include <WiFiClientSecure.h>
#include <PubSubClient.h>

WiFiClientSecure wifiClient;
PubSubClient     mqttClient(wifiClient);
MQTTOTA          ota;

const char* rootCA = \
"-----BEGIN CERTIFICATE-----\n" \
"... your CA certificate ...\n" \
"-----END CERTIFICATE-----\n";

void setup() {
    Serial.begin(115200);
    // ... WiFi setup ...

    wifiClient.setCACert(rootCA);
    mqttClient.setServer("your-secure-broker.com", 8883);
    mqttClient.setCallback([](char* topic, byte* payload, unsigned int len) {
        String msg;
        for (unsigned int i = 0; i < len; i++) msg += (char)payload[i];
        ota.processMessage(String(topic), msg);
    });
    // ... connect and subscribe ...

    ota.begin("MySecureDevice", "1.0.0");
    // Recommended in production:
    ota.setSecurityKey("production-hmac-secret-key-here");
    ota.setSecurityMode(SECURITY_HMAC_SHA256);

    ota.setMQTTConfig(
        [](const char* t, const String& m) { mqttClient.publish(t, m.c_str()); },
        []() { return mqttClient.connected(); },
        "devices/secure/ota"
    );
}

void loop() {
    mqttClient.loop();
    ota.handle();
}
```

---

## Message Formats

### Complete OTA Message (single-message mode)
```json
{
  "EventType": "UpdateFirmwareDevice",
  "Details": {
    "FirmwareVersion": "1.1.0",
    "Base64": "<complete_firmware_base64>",
    "IsError": false,
    "ErrorMessage": null
  }
}
```

### Chunked OTA Message
```json
{
  "EventType": "UpdateFirmwareDevice",
  "Details": {
    "FirmwareVersion": "1.2.0",
    "Base64Part": "<chunk_base64>",
    "PartIndex": 1,
    "TotalParts": 10,
    "IsError": false,
    "ErrorMessage": null,
    "sha256": "<expected_sha256_hex>",
    "hmac_sig": "<expected_hmac_hex>",
    "ecdsa_sig": "<expected_ecdsa_base64>"
  }
}
```

### Response Messages (published by the device)

```json
// ota/progress  (throttled to multiples of 10%)
{ "device": "ABC123", "version": "1.1.0", "progress": 50, "timestamp": 123456 }

// ota/error
{ "device": "ABC123", "version": "1.1.0", "error": "SHA-256 mismatch", "timestamp": 123456 }

// ota/success
{ "device": "ABC123", "version": "1.1.0", "success": true, "timestamp": 123456 }

// ota/state  (every state transition)
{ "device": "ABC123", "state": 5, "state_name": "WRITING", "timestamp": 123456 }
```

---

## Advanced Configuration

### Full configuration example

```cpp
void setup() {
    ota.begin("MyDevice", "2.0.0");

    // Security (v1.2.0)
    ota.setSecurityKey("your-shared-secret");
    ota.requireSignature(true);

    // Transfer mode
    ota.enableChunkedOTA(true);   // Default: true
    ota.setChunkSize(2048);

    // Resilience
    ota.setMaxRetries(5);
    ota.setAutoReset(true);       // Schedule non-blocking restart after OTA
    ota.enableVersionCheck(true); // Reject if version matches current
    ota.enableRollbackProtection(true);

    // MQTT
    ota.setMQTTConfig(publishFn, connectedFn, "devices/my/ota");

    // Callbacks
    ota.onProgress([](int pct, const String& ver) { /* ... */ });
    ota.onError([](const String& err, const String& ver) { /* ... */ });
    ota.onSuccess([](const String& ver) { /* ... */ });
    ota.onStateChange([](uint8_t state) {
        Serial.printf("OTA state changed: %d\n", state);
    });
}
```

### Memory monitoring

```cpp
void loop() {
    ota.handle();

    static unsigned long lastCheck = 0;
    if (millis() - lastCheck > 30000) {
        MQTTOTA::logMemoryStatus();         // free/min/maxAlloc heap
        Serial.printf("OTA space: %zu B\n", ota.getFreeOTASpace()); // v1.2.0
        lastCheck = millis();
    }

    if (!ota.isUpdateInProgress()) {
        handleSensors();
        publishTelemetry();
    }
}
```

---

## Diagnostics and Troubleshooting

### Built-in diagnostic dump

```cpp
ota.printDiagnostics();
// Output:
// === MQTTOTA Diagnostics ===
//   DeviceID : ESP32_OTA_TEST
//   Device   : ESP32_OTA_TEST
//   Version  : 1.0.0
//   State    : IDLE
//   Progress : 0%
//   Received : 0 B
//   Sig req  : yes
//   Key set  : yes
//   Heap — free=180000  minFree=120000  maxAlloc=90000
//   Running  : ota_0 @ 0x00010000
//   Stats — chunks=0 errors=0 speed=0.0 KB/s
// ===========================
```

### Common error handling

```cpp
ota.onError([](const String& error, const String& version) {
    if (error.indexOf("memory") != -1) {
        Serial.println("Low memory — free resources and retry");
        MQTTOTA::logMemoryStatus();
    } else if (error.indexOf("timeout") != -1) {
        Serial.println("OTA timed out — check MQTT connection");
    } else if (error.indexOf("SHA-256") != -1) {
        Serial.println("Integrity check failed — firmware rejected");
    } else if (error.indexOf("HMAC") != -1) {
        Serial.println("Authentication failed — check HMAC key");
    } else if (error.indexOf("sequence") != -1) {
        Serial.println("Chunk out of order — restart the transfer");
    }
});
```

### OTA state machine (v1.2.0)

```
IDLE
 └─► RECEIVING      (begin() partition found, esp_ota_begin OK)
      └─► DECODING  (base64 decoded successfully)
           └─► VALIDATING  (image header + SHA-256/HMAC check)
                └─► WRITING     (esp_ota_write per chunk)
                     └─► COMPLETING  (all chunks received)
                          ├─► SUCCESS  → pending non-blocking restart
                          └─► ERROR    → cleanup, back to IDLE
```

---

## Complete API

### Lifecycle

```cpp
void begin(const String& deviceName, const String& firmwareVersion);
// Also calls esp_ota_mark_app_valid_cancel_rollback() on OTA partitions (v1.2.0)

void handle();
// REQUIRED every loop() — drives restart timer and timeout watchdog (v1.2.0)

void setMQTTConfig(publishFn, isConnectedFn, otaTopic = "ota");
void setPartitionName(const String& partitionName = "");
```

### Security (v1.2.0)

```cpp
void setSecurityKey(const char* key);      // Set HMAC-SHA256 key (max 64 bytes)
void requireSignature(bool required = true); // Reject unsigned payloads if true

// Verify HMAC on raw decoded bytes (recommended)
bool verifyFirmwareSignature(const uint8_t* data, size_t len, const String& expectedHex);

// Legacy overload — backward compat, does not compute HMAC without raw data
bool verifyFirmwareSignature(const String& signature);
```

### OTA Configuration

```cpp
void enableChunkedOTA(bool enable = true);
void setChunkSize(size_t chunkSize);
void setAutoReset(bool autoReset = true);
void setMaxRetries(int maxRetries);
void enableVersionCheck(bool enable = true);
void enableRollbackProtection(bool enable = true);
```

### Processing

```cpp
void processMessage(const String& topic, const String& message);
bool performUpdate(const String& base64Data, const String& firmwareVersion);
void abortUpdate();
```

### Status & Query

```cpp
bool      isUpdateInProgress() const;  // v1.2.0: single source of truth
bool      isValidating()       const;
bool      isWriting()          const;
OTAState  getCurrentState();           // Full 9-state enum
int       getProgress();
String    getCurrentVersion();
String    getDeviceID();
size_t    getFreeOTASpace();           // v1.2.0: was commented out
OTAStatistics getStatistics();         // bytes, chunks, errors, avgSpeed
String    getBootPartitionInfo();
void      printDiagnostics();
```

### Memory Utilities

```cpp
static bool   checkMemory(size_t requiredBytes);
static size_t getFreeHeap();
static void   logMemoryStatus();
```

### Callbacks

```cpp
void onProgress(MQTTOTACallback callback);           // (int pct, const String& ver)
void onError(MQTTOTAErrorCallback callback);         // (const String& err, const String& ver)
void onSuccess(MQTTOTASuccessCallback callback);     // (const String& ver)
void onStateChange(MQTTOTAStateCallback callback);   // (uint8_t state)
```

### OTA State Enum

```cpp
enum OTAState {
    OTA_STATE_IDLE       = 0,
    OTA_STATE_RECEIVING  = 1,
    OTA_STATE_DECODING   = 2,
    OTA_STATE_VALIDATING = 3,
    OTA_STATE_WRITING    = 4,
    OTA_STATE_COMPLETING = 5,
    OTA_STATE_SUCCESS    = 6,
    OTA_STATE_ERROR      = 7,
    OTA_STATE_ABORTED    = 8
};
```

---

## Performance Considerations

### Chunked vs. full mode

| Mode | Heap impact | Recommended for |
|---|---|---|
| Full (single message) | ~8 KB stack (StaticJson) + decoded size in RAM | Firmware < 100 KB |
| Chunked | ~4 KB stack (StaticJson) + 1 chunk in RAM | Any firmware size |

### Throughput tips

```cpp
// Larger chunks = better throughput (up to MQTT_OTA_MAX_CHUNK_SIZE = 64 KB)
ota.setChunkSize(16384);  // 16 KB chunks

// Reduce retries to fail faster on unstable links
ota.setMaxRetries(2);

// Track average speed
OTAStatistics s = ota.getStatistics();
Serial.printf("Speed: %.1f KB/s\n", s.avgSpeedBps / 1024.0f);
```

### Handling unstable connections

```cpp
ota.onError([](const String& error, const String& version) {
    if (error.indexOf("timeout") != -1 || error.indexOf("sequence") != -1) {
        Serial.println("Transfer interrupted — will retry on next OTA message");
        // No manual restart needed; state is already IDLE after cleanup
    }
});
```

---

## Best Practices

### 1. Always use MQTTS + HMAC in production

```cpp
// Transport encryption
wifiClient.setCACert(rootCA);

// Firmware authentication (v1.2.0)
ota.setSecurityKey("production-secret-min-32-chars");
ota.requireSignature(true);
```

### 2. Check memory before triggering OTA

```cpp
bool canStartOTA() {
    return (ESP.getFreeHeap() > 50000) &&
           (WiFi.status() == WL_CONNECTED) &&
           (!ota.isUpdateInProgress());
}
```

### 3. Always call handle() in loop()

```cpp
void loop() {
    mqttClient.loop();
    ota.handle();   // Never skip this — non-blocking restart depends on it
}
```

### 4. Use printDiagnostics() before shipping

```cpp
// In setup(), after begin(), log the full state
ota.printDiagnostics();
// Confirms partition, rollback status, HMAC key, and heap
```

---

## Complete Workflows

### Successful chunked OTA (v1.2.0 flow)

```
Backend                          ESP32 Device
  │                                  │
  │─── chunk 1/10 (partIndex=1) ────►│  esp_ota_begin()
  │◄── ota/progress: 10% ────────────│  RECEIVING → DECODING → VALIDATING → WRITING
  │                                  │
  │─── chunk 2/10 ──────────────────►│  esp_ota_write()
  │◄── ota/progress: 20% ────────────│  WRITING
  │  ... (chunks 3–9) ...            │
  │─── chunk 10/10 ─────────────────►│  esp_ota_end()
  │◄── ota/progress: 95% ────────────│  COMPLETING → VALIDATING (SHA-256) → SUCCESS
  │◄── ota/success ──────────────────│  schedule restart (3 s timer)
  │                                  │  ... handle() fires restart ...
  │                                  │  ESP.restart()
```

### OTA error flow

```
Backend                          ESP32 Device
  │                                  │
  │─── chunk 3/10 ──────────────────►│  Out-of-sequence (expected 4)
  │◄── ota/error: "Chunk out of      │  esp_ota_abort()
  │      sequence" ──────────────────│  State → IDLE, ready for retry
```

---

## Backend Implementation

To use MQTTOTA in your project, you'll need an MQTT server to manage OTA updates. You can implement your own backend using our reference repository:

**MQTT Broker for OTA Updates**
- **Repository:** [github.com/Ruben890/Mqtt-Broker](https://github.com/Ruben890/Mqtt-Broker)
- **Description:** Complete backend for managing OTA updates via MQTT/MQTTS
- **Features:**
  - Configurable MQTT server
  - IoT device management
  - Firmware update delivery
  - OTA progress tracking
  - Error handling and retry mechanisms

**Steps to use the backend:**
1. Clone the backend repository
2. Configure the MQTT broker according to your needs
3. Implement the update delivery logic
4. Connect your ESP32 devices to the broker
5. Manage OTA updates from a centralized interface

**Example workflow:**
```javascript
// From your backend
1. Prepare firmware in base64 format
2. Publish MQTT message to target device
3. Monitor progress via callbacks
4. Confirm successful completion
5. Log results in database
```

The backend provides a scalable architecture for managing multiple devices simultaneously, with support for mass updates and firmware version management.

---

## Broker Compatibility

Tested with:
- [Mosquitto](https://mosquitto.org/) 2.0+
- [EMQX](https://www.emqx.com/) 5.0+
- [HiveMQ Cloud](https://www.hivemq.com/)
- [AWS IoT Core](https://aws.amazon.com/iot-core/)
- ESP-IDF MQTT client (used in `BasicOTA` example)

---

## License

Licensed under the **MIT License**. See [LICENSE](LICENSE) for details.

---

## Contact

Author: **Jorge Gaspar Beltre Rivera**  
Project: **MQTTOTA - For OTA Updates via MQTT/MQTTS**

<p align="center">
  <a href="https://www.linkedin.com/in/jorge-gaspar-beltre-rivera/" target="_blank"><img src="https://user-images.githubusercontent.com/74038190/235294012-0a55e343-37ad-4b0f-924f-c8431d9d2483.gif" alt="LinkedIn" width="100"></a>
  <a href="https://github.com/JorgeGBeltre" target="_blank"><img src="https://user-images.githubusercontent.com/74038190/212257468-1e9a91f1-b626-4baa-b15d-5c385dfa7ed2.gif" alt="GitHub" width="100"></a>
  <a href="mailto:Jorgegaspar3021@gmail.com"><img src="https://user-images.githubusercontent.com/74038190/216122065-2f028bae-25d6-4a3c-bc9f-175394ed5011.png" alt="E-Mail" width="100"></a>

</p>

## Support

This project is developed independently. Even a small contribution helps me dedicate more time to development, testing, and releasing new features.


 <p align="center">
  <a href="https://www.paypal.com/donate/?hosted_button_id=2VLA8BWT967LU">
    <img src="https://www.paypalobjects.com/webstatic/icon/pp258.png"
         alt="Donate with PayPal"
         height="60">
  </a>
</p>

