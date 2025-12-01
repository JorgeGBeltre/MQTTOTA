# MQTTOTA - For OTA Updates via MQTT/MQTTS

**MQTTOTA** is an SDK that revolutionizes firmware management for ESP32-based IoT devices. By leveraging the power of MQTT/MQTTS protocols, it provides a seamless, secure, and scalable solution for Over-The-Air updates in distributed IoT ecosystems. Whether you're managing a handful of devices or thousands across global deployments, MQTTOTA ensures reliable firmware delivery with enterprise-level security and robust error handling.

## Table of Contents

- [Overview](#overview)
- [Key Features](#key-features)
  - [Update Methods](#update-methods)
  - [Security](#security)
  - [Monitoring and Control](#monitoring-and-control)
- [Installation](#installation)
  - [Method 1: Manual Installation](#method-1-manual-installation)
  - [Method 2: Using PlatformIO](#method-2-using-platformio)
- [Dependencies](#dependencies)
  - [Required Libraries](#required-libraries)
  - [Memory Configuration](#memory-configuration)
- [Basic Configuration](#basic-configuration)
  - [Minimum Initialization](#minimum-initialization)
- [Standard MQTT Configuration](#standard-mqtt-configuration)
  - [Example with PubSubClient](#example-with-pubsubclient)
- [MQTTS (Secure) Configuration](#mqtts-secure-configuration)
  - [Example with MQTTS and Certificates](#example-with-mqtts-and-certificates)
- [Message Formats](#message-formats)
  - [Complete OTA Message](#complete-ota-message)
  - [Chunked OTA Message](#chunked-ota-message)
  - [Response Messages](#response-messages)
- [Advanced Configuration](#advanced-configuration)
  - [Parameter Customization](#parameter-customization)
  - [Advanced Memory Management](#advanced-memory-management)
- [Diagnostics and Troubleshooting](#diagnostics-and-troubleshooting)
  - [Enable Detailed Logs](#enable-detailed-logs)
  - [Common Error Handling](#common-error-handling)
- [Complete API](#complete-api)
  - [Public Methods](#public-methods)
    - [Lifecycle Management](#lifecycle-management)
    - [Message Processing](#message-processing)
    - [Chunked OTA Configuration](#chunked-ota-configuration)
    - [Status Query](#status-query)
    - [Utilities](#utilities)
  - [Available Callbacks](#available-callbacks)
    - [Update Progress](#update-progress)
    - [Error Handling](#error-handling)
    - [Successful Completion](#successful-completion)
- [Performance Considerations](#performance-considerations)
  - [Memory Optimization](#memory-optimization)
  - [Handling Unstable Connections](#handling-unstable-connections)
- [Best Practices](#best-practices)
- [Complete Workflows](#complete-workflows)
  - [Successful OTA Flow](#successful-ota-flow)
  - [OTA Error Flow](#ota-error-flow)
- [Support and Contributions](#support-and-contributions)
  - [Reporting Issues](#reporting-issues)
  - [Development Best Practices](#development-best-practices)
- [Contact](#contact)

## Overview

**MQTTOTA** is a robust and comprehensive SDK that enables secure Over-The-Air (OTA) firmware updates using MQTT and MQTTS protocols. Specifically designed for ESP32-based IoT devices, it offers multiple update methods with error handling, progress tracking, and recovery capabilities.


## Key Features

### Update Methods
- **Full OTA**: Update with a single MQTT message
- **Chunked OTA**: Fragmented transfer for large firmware files
- **Native ESP-IDF OTA**: Robust implementation using native ESP32 APIs
- **Firmware Validation**: Integrity and compatibility verification

### Security
- **MQTTS Supported**: Encrypted communication with TLS certificates
- **Base64 Validation**: Firmware data verification
- **SHA-256 Checksum**: Image integrity verification
- **Configurable Timeout**: Protection against hung updates

### Monitoring and Control
- **Event Callbacks**: Progress, success, and error
- **Progress Tracking**: Real-time reporting
- **System Status**: Progress and version query
- **Detailed Logs**: Comprehensive diagnostic information

## Installation

### Method 1: Manual Installation
1. Download `MQTTOTA.h` and `MQTTOTA.cpp` files
2. Create `MQTTOTA` folder in `Arduino/libraries/`
3. Copy files to the folder
4. Restart Arduino IDE

### Method 2: Using PlatformIO
```ini
lib_deps =
    https://github.com/JorgeBeltre/MQTTOTA.git
```

## Dependencies

### Required Libraries
```cpp
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>        // v6.19+
#include <Update.h>
#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include "esp_partition.h"
```

### Memory Configuration
```cpp
// Adjust according to your device
#define MQTT_OTA_JSON_SIZE 32768    // For large messages
#define MQTT_OTA_BUFFSIZE 1024      // Chunk size
```

## Basic Configuration

### Minimum Initialization
```cpp
#include <MQTTOTA.h>

MQTTOTA ota;

void setup() {
    Serial.begin(115200);
    
    // Basic configuration
    ota.begin("MyDevice", "1.0.0");
}

void loop() {
    ota.handle();
}
```

## Standard MQTT Configuration

### Example with PubSubClient
```cpp
#include <MQTTOTA.h>
#include <PubSubClient.h>
#include <WiFi.h>

WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);
MQTTOTA ota;

// WiFi configuration
const char* ssid = "your_SSID";
const char* password = "your_PASSWORD";

// MQTT configuration
const char* mqttServer = "broker.hivemq.com";
const int mqttPort = 1883;
const char* mqttTopic = "devices/my_device/ota";

void setupWiFi() {
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
        delay(1000);
        Serial.println("Connecting to WiFi...");
    }
    Serial.println("WiFi connected");
}

void setupMQTT() {
    mqttClient.setServer(mqttServer, mqttPort);
    mqttClient.setCallback(mqttCallback);
    
    while (!mqttClient.connected()) {
        if (mqttClient.connect("my_device")) {
            mqttClient.subscribe(mqttTopic);
            Serial.println("MQTT connected");
        } else {
            delay(5000);
        }
    }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
    String message;
    for (int i = 0; i < length; i++) {
        message += (char)payload[i];
    }
    
    // Process OTA message
    ota.processMessage(String(topic), message);
}

void setup() {
    Serial.begin(115200);
    
    setupWiFi();
    setupMQTT();
    
    // Configure MQTTOTA
    ota.begin("MyDevice", "1.0.0");
    ota.setMQTTConfig(
        // Function to publish
        [](const char* topic, const String& message) {
            mqttClient.publish(topic, message.c_str());
        },
        // Function to check connection
        []() {
            return mqttClient.connected();
        },
        // OTA topic (optional, default "ota")
        mqttTopic
    );
    
    // Configure callbacks
    ota.onProgress([](int progress, const String& version) {
        Serial.printf("OTA Progress: %d%% (v%s)\n", progress, version.c_str());
    });
    
    ota.onError([](const String& error, const String& version) {
        Serial.printf("OTA Error: %s (v%s)\n", error.c_str(), version.c_str());
    });
    
    ota.onSuccess([](const String& version) {
        Serial.printf("OTA Completed: v%s\n", version.c_str());
    });
}

void loop() {
    if (!mqttClient.connected()) {
        setupMQTT();
    }
    mqttClient.loop();
    ota.handle();
}
```

## MQTTS (Secure) Configuration

### Example with MQTTS and Certificates
```cpp
#include <MQTTOTA.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>

WiFiClientSecure wifiClient;
PubSubClient mqttClient(wifiClient);
MQTTOTA ota;

// MQTTS configuration
const char* mqttServer = "your-secure-server.com";
const int mqttPort = 8883;
const char* mqttUser = "username";
const char* mqttPassword = "password";

// CA Certificate (optional, for strict verification)
const char* rootCA = \
"-----BEGIN CERTIFICATE-----\n" \
"MIIDQTCCAimgAwIBAgITBmyfz5m/jAo54vB4ikPmljZbyjANBgkqhkiG9w0BAQsF\n" \
// ... your complete CA certificate
"-----END CERTIFICATE-----\n";

void setupMQTTS() {
    // Configure secure client
    wifiClient.setCACert(rootCA);
    // wifiClient.setCertificate(client_cert);  // For client certificate
    // wifiClient.setPrivateKey(client_key);    // For private key
    
    mqttClient.setServer(mqttServer, mqttPort);
    mqttClient.setCallback(mqttCallback);
    
    // Connect with authentication
    while (!mqttClient.connected()) {
        if (mqttClient.connect("my_secure_device", mqttUser, mqttPassword)) {
            mqttClient.subscribe("devices/secure/ota");
            Serial.println("MQTTS securely connected");
        } else {
            Serial.print("MQTTS Error: ");
            Serial.println(mqttClient.state());
            delay(5000);
        }
    }
}

void setup() {
    Serial.begin(115200);
    setupWiFi();  // Same WiFi function from previous example
    
    setupMQTTS();
    
    // Configure MQTTOTA same as normal MQTT
    ota.begin("MySecureDevice", "1.0.0");
    ota.setMQTTConfig(
        [](const char* topic, const String& message) {
            mqttClient.publish(topic, message.c_str());
        },
        []() {
            return mqttClient.connected();
        },
        "devices/secure/ota"
    );
    
    // Callbacks work the same
    ota.onProgress(progressCallback);
    ota.onError(errorCallback);
    ota.onSuccess(successCallback);
}
```

## Message Formats

### Complete OTA Message
```json
{
  "EventType": "UpdateFirmwareDevice",
  "Details": {
    "FirmwareVersion": "1.1.0",
    "Base64": "base64_encoded_firmware_data_here...",
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
    "FirmwareVersion": "1.1.0",
    "Base64Part": "chunk_base64_data_here...",
    "PartIndex": 1,
    "TotalParts": 10,
    "IsError": false,
    "ErrorMessage": null
  }
}
```

### Response Messages
```json
// Progress
{
  "device": "ABC123",
  "version": "1.1.0",
  "progress": 45,
  "timestamp": 1234567890
}

// Error
{
  "device": "ABC123",
  "version": "1.1.0",
  "error": "Update timeout",
  "timestamp": 1234567890
}

// Success
{
  "device": "ABC123",
  "version": "1.1.0",
  "success": true,
  "timestamp": 1234567890
}
```

## Advanced Configuration

### Parameter Customization
```cpp
void setup() {
    ota.begin("MyDevice", "2.0.0");
    
    // Enable/disable chunked OTA
    ota.enableChunkedOTA(true);  // Default true
    
    // Configure chunk size
    ota.setChunkSize(2048);  // 2KB per chunk
    
    // Configure advanced callbacks
    ota.onProgress([](int progress, const String& version) {
        // Publish progress to dashboard
        publishToDashboard("ota_progress", progress);
        
        // Control status LED
        if (progress < 100) {
            digitalWrite(LED_PIN, progress % 2);  // Blinking
        } else {
            digitalWrite(LED_PIN, HIGH);  // Solid on
        }
    });
    
    ota.onError([](const String& error, const String& version) {
        // Log error in logging system
        logError("OTA_FAILED", error);
        
        // Send notification
        sendNotification("OTA Error: " + error);
        
        // Retry after error
        if (error.indexOf("timeout") != -1) {
            delay(30000);
            attemptReconnect();
        }
    });
}
```

### Advanced Memory Management
```cpp
void checkSystemResources() {
    Serial.printf("Free memory: %d\n", ESP.getFreeHeap());
    Serial.printf("OTA in progress: %s\n", ota.isUpdateInProgress() ? "Yes" : "No");
    Serial.printf("Current version: %s\n", ota.getCurrentVersion());
    Serial.printf("Current progress: %d%%\n", ota.getProgress());
}

// In your main loop
void loop() {
    static unsigned long lastCheck = 0;
    
    ota.handle();
    
    if (millis() - lastCheck > 30000) {
        checkSystemResources();
        lastCheck = millis();
    }
    
    // Only process other tasks if no OTA in progress
    if (!ota.isUpdateInProgress()) {
        handleSensors();
        handleUserInput();
        publishTelemetry();
    }
}
```

## Diagnostics and Troubleshooting

### Enable Detailed Logs
```cpp
void setup() {
    Serial.begin(115200);
    
    // Verify OTA partitions
    const esp_partition_t* running = esp_ota_get_running_partition();
    const esp_partition_t* update = esp_ota_get_next_update_partition(NULL);
    
    Serial.printf("Current partition: %s\n", running->label);
    Serial.printf("Update partition: %s\n", update->label);
    Serial.printf("Available size: %d bytes\n", update->size);
}

// OTA diagnostic function
void printOTADiagnostics() {
    Serial.println("=== OTA DIAGNOSTICS ===");
    Serial.printf("Free memory: %d bytes\n", ESP.getFreeHeap());
    Serial.printf("OTA in progress: %s\n", ota.isUpdateInProgress() ? "YES" : "NO");
    Serial.printf("Firmware version: %s\n", ota.getCurrentVersion());
    Serial.printf("Device ID: %s\n", ota.getDeviceID());
    
    if (ota.isUpdateInProgress()) {
        Serial.printf("Progress: %d%%\n", ota.getProgress());
    }
}
```

### Common Error Handling
```cpp
void handleOTAErrors() {
    ota.onError([](const String& error, const String& version) {
        Serial.printf("OTA error detected: %s\n", error.c_str());
        
        if (error.indexOf("memory") != -1) {
            Serial.println("Freeing memory...");
            freeUnusedResources();
        } else if (error.indexOf("timeout") != -1) {
            Serial.println("Reconnecting...");
            reconnectNetwork();
        } else if (error.indexOf("partition") != -1) {
            Serial.println("Verify OTA partitions");
            checkPartitions();
        }
        
        // Clean OTA state
        ota.cleanup();
    });
}
```

## Complete API

### Public Methods

#### Lifecycle Management
```cpp
// Initialization
void begin(const String& deviceName, const String& firmwareVersion);

// MQTT Configuration
void setMQTTConfig(
    std::function<void(const char* topic, const String& message)> publishFunc,
    std::function<bool()> isConnectedFunc,
    const String& otaTopic = "ota"
);

// Main handling
void handle();
```

#### Message Processing
```cpp
// Process MQTT messages
void processMessage(const String& topic, const String& message);

// Perform manual update
bool performUpdate(const String& base64Data, const String& firmwareVersion);
```

#### Chunked OTA Configuration
```cpp
// Enable/disable chunks
void enableChunkedOTA(bool enable = true);

// Configure chunk size
void setChunkSize(size_t chunkSize);
```

#### Status Query
```cpp
// Current status
bool isUpdateInProgress();
String getCurrentVersion();
String getDeviceID();
int getProgress();
```

#### Utilities
```cpp
// State cleanup
void cleanup();

// Base64 encoding (static)
static String base64Decode(const String& encoded);
static String base64Encode(const String& input);
```

### Available Callbacks

#### Update Progress
```cpp
void onProgress(MQTTOTACallback callback);
// Example: ota.onProgress([](int progress, const String& version) { ... });
```

#### Error Handling
```cpp
void onError(MQTTOTAErrorCallback callback);
// Example: ota.onError([](const String& error, const String& version) { ... });
```

#### Successful Completion
```cpp
void onSuccess(MQTTOTASuccessCallback callback);
// Example: ota.onSuccess([](const String& version) { ... });
```

## Performance Considerations

### Memory Optimization
```cpp
// For memory-limited devices
#define MQTT_OTA_JSON_SIZE 16384
#define MQTT_OTA_BUFFSIZE 512

void setup() {
    // Reduce chunk size
    ota.setChunkSize(512);
    
    // Monitor memory
    Serial.printf("Initial memory: %d\n", ESP.getFreeHeap());
}
```

### Handling Unstable Connections
```cpp
void robustOTAHandling() {
    ota.onError([](const String& error, const String& version) {
        if (error.indexOf("timeout") != -1 || error.indexOf("connection") != -1) {
            Serial.println("Retrying in 30 seconds...");
            delay(30000);
            ESP.restart();  // Or reconnect smoothly
        }
    });
}
```

## Best Practices

### 1. **Always Use MQTTS in Production**
```cpp
// In production, always use encryption
wifiClient.setCACert(rootCA);
// Never send firmware in clear text
```

### 2. **Validate Before Updating**
```cpp
// Verify resources before OTA
bool canStartOTA() {
    return (ESP.getFreeHeap() > 50000) && 
           (WiFi.status() == WL_CONNECTED) &&
           (!ota.isUpdateInProgress());
}
```

### 3. **Graceful Failure Handling**
```cpp
ota.onError([](const String& error, const String& version) {
    // Don't restart immediately, allow recovery
    logError("OTA_FAILED", error);
    
    // Wait and retry
    if (autoRetryEnabled) {
        delay(60000);
        attemptRecovery();
    }
});
```

### 4. **Backup and Rollback**
```cpp
// Verify previous partition after OTA
void checkBootPartition() {
    const esp_partition_t* running = esp_ota_get_running_partition();
    Serial.printf("Booted from: %s\n", running->label);
}
```

## Complete Workflows

### Successful OTA Flow
1. Device connected to WiFi and MQTT
2. Receives valid OTA message
3. Validates resources and firmware
4. Starts update with progress
5. Verifies image and writes partition
6. Configures new boot partition
7. Restarts and loads new firmware
8. Confirms success to server

### OTA Error Flow
1. Device receives OTA message
2. Detects insufficient resources
3. Publishes specific error
4. Cleans internal state
5. Allows later retry
6. Continues normal operation

## Support and Contributions

### Reporting Issues
Always include:
- SDK version
- Platform (Arduino IDE/PlatformIO)
- Complete error logs
- MQTT/MQTTS configuration
- Message causing the issue

### Development Best Practices
```cpp
// Example of well-structured code
void handleOTAMessage(String topic, String message) {
    if (!ota.isUpdateInProgress() && 
        hasSufficientMemory() && 
        isStableConnection()) {
        ota.processMessage(topic, message);
    } else {
        deferOTAMessage(topic, message);
    }
}
```

## Contact

Author: **Jorge Gaspar Beltre Rivera**  
Project: MQTTOTA - For OTA Updates via MQTT/MQTTS

GitHub: [github.com/JorgeBeltre](https://github.com/JorgeGBeltre)


