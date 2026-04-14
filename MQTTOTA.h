#ifndef MQTT_OTA_SDK_H
#define MQTT_OTA_SDK_H

#include "esp_app_format.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <mbedtls/md.h>
#include <mbedtls/sha256.h>

extern "C" {
#include "libb64/cdecode.h"
#include "libb64/cencode.h"
}

// Configuration macros

#ifndef MQTT_OTA_BUFFSIZE
#define MQTT_OTA_BUFFSIZE 1024
#endif

#ifndef MQTT_OTA_HASH_LEN
#define MQTT_OTA_HASH_LEN 32
#endif

#ifndef MQTT_OTA_TIMEOUT_MS
#define MQTT_OTA_TIMEOUT_MS 420000 // 7 minutes
#endif

// JSON document size for the full-firmware mode.
// NOTE: StaticJsonDocument allocates on the stack (~8 KB in loop()).
// If you use chunked mode exclusively, this value has no impact.
#ifndef MQTT_OTA_JSON_SIZE
#define MQTT_OTA_JSON_SIZE 8192
#endif

#ifndef MQTT_OTA_MAX_CHUNK_SIZE
#define MQTT_OTA_MAX_CHUNK_SIZE 65536 // 64 KB maximum per chunk
#endif

#ifndef MQTT_OTA_MIN_MEMORY
#define MQTT_OTA_MIN_MEMORY 40000 // Minimum free heap before OTA
#endif

#ifndef MQTT_OTA_MAX_RETRIES
#define MQTT_OTA_MAX_RETRIES 3
#endif

// Maximum HMAC key length (bytes)
#ifndef MQTT_OTA_HMAC_KEY_SIZE
#define MQTT_OTA_HMAC_KEY_SIZE 64
#endif

// Non-blocking restart delay after successful OTA (ms).

#ifndef MQTT_OTA_RESTART_DELAY_MS
#define MQTT_OTA_RESTART_DELAY_MS 3000UL
#endif

// Callbacks

typedef std::function<void(int progress, const String &version)>
    MQTTOTACallback;
typedef std::function<void(const String &error, const String &version)>
    MQTTOTAErrorCallback;
typedef std::function<void(const String &version)> MQTTOTASuccessCallback;
typedef std::function<void(uint8_t state)> MQTTOTAStateCallback;

// OTA States

enum OTAState {
  OTA_STATE_IDLE = 0,
  OTA_STATE_RECEIVING = 1,
  OTA_STATE_DECODING = 2,
  OTA_STATE_VALIDATING = 3,
  OTA_STATE_WRITING = 4,
  OTA_STATE_COMPLETING = 5,
  OTA_STATE_SUCCESS = 6,
  OTA_STATE_ERROR = 7,
  OTA_STATE_ABORTED = 8
};

// OTA Statistics

struct OTAStatistics {
  unsigned long startTime = 0;
  unsigned long endTime = 0;
  size_t totalBytes = 0;
  size_t receivedBytes = 0;
  int chunkCount = 0;
  int errorCount = 0;
  OTAState lastState = OTA_STATE_IDLE;
  String lastError = "";
  float averageSpeed = 0.0f; // bytes/second
};

// Main MQTTOTA class

class MQTTOTA {
public:
  // Construction
  MQTTOTA();
  ~MQTTOTA();

  // Configuration

  void begin(const String &deviceName, const String &firmwareVersion);

  void setMQTTConfig(
      std::function<void(const char *topic, const String &message)> publishFunc,
      std::function<bool()> isConnectedFunc, const String &otaTopic = "ota");

  void setPartitionName(const String &partitionName = "");

  // Security

  void setSecurityKey(const char *key);

  void requireSignature(bool required = true);

  // Callbacks

  void onProgress(MQTTOTACallback callback);
  void onError(MQTTOTAErrorCallback callback);
  void onSuccess(MQTTOTASuccessCallback callback);
  void onStateChange(MQTTOTAStateCallback callback);

  // Main methods

  void handle();

  void processMessage(const String &topic, const String &message);
  bool performUpdate(const String &base64Data, const String &firmwareVersion);

  // OTA configuration

  void enableChunkedOTA(bool enable = true);
  void setChunkSize(size_t chunkSize);
  void setAutoReset(bool autoReset = true);
  void setMaxRetries(int maxRetries);
  void enableRollbackProtection(bool enable = true);
  void enableVersionCheck(bool enable = true);

  // Status and queries

  bool isUpdateInProgress() const;
  bool isValidating() const;
  bool isWriting() const;
  String getCurrentVersion();
  String getDeviceID();
  int getProgress();
  OTAState getCurrentState();
  OTAStatistics getStatistics();

  size_t getFreeOTASpace();

  // Utilities and diagnostics

  void printDiagnostics();
  String getBootPartitionInfo();

  static String base64Decode(const String &encoded);
  static String base64Encode(const String &input);
  static size_t calculateBase64DecodedSize(const String &encoded);

  void cleanup();
  void abortUpdate();

  // Memory management

  static bool checkMemory(size_t requiredBytes);
  static size_t getFreeHeap();
  static void logMemoryStatus();

  // Security helpers

  bool verifyFirmwareSignature(const uint8_t *data, size_t len,
                               const String &expectedHex);
  bool verifyFirmwareSignature(const String &signature);

  bool checkFirmwareCompatibility(const String &newVersion);

private:
  // Internal structures

  struct OTAContext {
    bool inProgress = false;
    String firmwareVersion;
    int currentPart = 0;
    int totalParts = 0;
    unsigned long startTime = 0;
    size_t receivedSize = 0;
    esp_ota_handle_t update_handle = 0;
    const esp_partition_t *update_partition = nullptr;
    OTAState state = OTA_STATE_IDLE;
    int retryCount = 0;
    int maxRetries = MQTT_OTA_MAX_RETRIES;
    String partitionName;
    bool rollbackEnabled = true;
    bool versionCheckEnabled = true;

    // Non-blocking restart
    bool pendingRestart = false;
    unsigned long restartAt = 0;
  };

  struct OTAChunkData {
    String firmwareVersion;
    String base64Part;
    int partIndex = 0;
    int totalParts = 0;
    bool isError = false;
    String errorMessage;
    String checksum;
    size_t decodedSize = 0;
  };

  // Member variables

  String _deviceName;
  String _firmwareVersion;
  String _deviceID;
  String _otaTopic;

  OTAContext _otaContext;
  OTAStatistics _stats;
  int _currentProgress = 0;

  // HMAC-SHA256 security
  uint8_t _hmacKey[MQTT_OTA_HMAC_KEY_SIZE];
  size_t _hmacKeyLen = 0;
  bool _requireSig = false; // false = backward-compatible default

  // Callbacks
  MQTTOTACallback _progressCallback = nullptr;
  MQTTOTAErrorCallback _errorCallback = nullptr;
  MQTTOTASuccessCallback _successCallback = nullptr;
  MQTTOTAStateCallback _stateChangeCallback = nullptr;

  // MQTT transport
  std::function<void(const char *topic, const String &message)> _publishMQTT =
      nullptr;
  std::function<bool()> _isMQTTConnected = nullptr;

  // OTA mode configuration
  bool _chunkedOTAEnabled = true;
  bool _autoReset = true;
  size_t _chunkSize = MQTT_OTA_BUFFSIZE;

  // Private methods

  void _initialize();
  void _processOTAMessage(const String &message);
  void _processOTAChunk(const String &message);

  bool _validateFirmwareData(const String &base64Data);
  bool _validateChecksum(const String &data, const String &checksum);
  bool _performOTAUpdateESPIDF(const String &base64Data,
                               const String &firmwareVersion);

  // Chunked OTA flow
  bool _startChunkedOTA(const OTAChunkData &chunk);
  bool _processChunkData(const OTAChunkData &chunk);
  void _completeChunkedOTA(const OTAChunkData &chunk);
  void _cleanupChunkedOTA();
  void _handleChunkError(const OTAChunkData &chunk, const String &error);

  // Publishing
  void _publishError(const String &errorMessage,
                     const String &firmwareVersion = "");
  void _publishSuccess(const String &firmwareVersion);
  void _publishProgress(int progress, const String &firmwareVersion);
  void _publishStateChange(OTAState state);

  // Utilities
  static void _printSHA256(const uint8_t *image_hash, const char *label);
  static bool _processImageHeader(const uint8_t *data, size_t data_len);
  static String _calculateSHA256(const uint8_t *data, size_t length);
  String _hmacSha256Hex(const uint8_t *data, size_t len) const;
  String _generateDeviceID();

  // State management
  void _setState(OTAState state);
  void _updateStatistics(size_t bytesReceived = 0, bool isError = false);

  // Validation
  bool _checkFirmwareVersion(const String &newVersion);
  bool _checkRollbackProtection();
  bool _verifyImageIntegrity(const uint8_t *data, size_t length);
  bool _validatePartitionWrite();

  // Helper
  String _getStateName(OTAState state);
};

// Inline implementations

inline bool MQTTOTA::isUpdateInProgress() const {
  return _otaContext.inProgress;
}

inline bool MQTTOTA::isValidating() const {
  return _otaContext.state == OTA_STATE_VALIDATING;
}

inline bool MQTTOTA::isWriting() const {
  return _otaContext.state == OTA_STATE_WRITING;
}

inline String MQTTOTA::getCurrentVersion() { return _firmwareVersion; }

inline String MQTTOTA::getDeviceID() { return _deviceID; }

inline int MQTTOTA::getProgress() { return _currentProgress; }

inline void MQTTOTA::enableChunkedOTA(bool enable) {
  _chunkedOTAEnabled = enable;
}

inline void MQTTOTA::setChunkSize(size_t chunkSize) {
  _chunkSize = (chunkSize > 0 && chunkSize <= MQTT_OTA_MAX_CHUNK_SIZE)
                   ? chunkSize
                   : MQTT_OTA_BUFFSIZE;
}

inline void MQTTOTA::setAutoReset(bool autoReset) { _autoReset = autoReset; }

inline void MQTTOTA::setMaxRetries(int maxRetries) {
  _otaContext.maxRetries = (maxRetries > 0) ? maxRetries : MQTT_OTA_MAX_RETRIES;
}

#endif // MQTT_OTA_SDK_H