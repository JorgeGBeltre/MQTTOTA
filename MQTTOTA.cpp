#include "MQTTOTA.h"
#include <Update.h>

extern "C" {
#include "libb64/cdecode.h"
#include "libb64/cencode.h"
}

// Base64 helpers (public static)

String MQTTOTA::base64Decode(const String &encoded) {
  if (ESP.getFreeHeap() < 35000) {
    Serial.println("[MQTTOTA] WARNING: low memory before Base64 decode");
    yield();
  }

  const char *encodedChars = encoded.c_str();
  int encodedLength = encoded.length();
  int maxDecodedSize = (encodedLength * 3) / 4 + 2;

  if (maxDecodedSize > (int)MQTT_OTA_MAX_CHUNK_SIZE) {
    Serial.printf("[MQTTOTA] ERROR: Base64 chunk too large: %d bytes\n",
                  maxDecodedSize);
    return "";
  }

  base64_decodestate state;
  base64_init_decodestate(&state);

  char *buffer = (char *)malloc(maxDecodedSize);
  if (!buffer) {
    Serial.println("[MQTTOTA] ERROR: malloc failed for Base64 buffer");
    return "";
  }

  int count = base64_decode_block(encodedChars, encodedLength, buffer, &state);

  String decoded;
  if (count > 0) {
    decoded = String(buffer, count);
  } else {
    Serial.println("[MQTTOTA] ERROR: Base64 decode returned 0 bytes");
  }

  free(buffer);
  return decoded;
}

String MQTTOTA::base64Encode(const String &input) {
  if (input.isEmpty())
    return "";

  const char *inputChars = input.c_str();
  int inputLength = input.length();
  int encodedLength = (inputLength + 2) / 3 * 4 + 1;

  char *buffer = (char *)malloc(encodedLength);
  if (!buffer)
    return "";

  base64_encodestate state;
  base64_init_encodestate(&state);

  int count = base64_encode_block(inputChars, inputLength, buffer, &state);
  count += base64_encode_blockend(buffer + count, &state);

  String encoded = String(buffer, count);
  free(buffer);
  return encoded;
}

// Constructor / Destructor

MQTTOTA::MQTTOTA() {
  memset(_hmacKey, 0, sizeof(_hmacKey));
  mbedtls_sha256_init(&_otaContext.sha256_ctx);
  mbedtls_md_init(&_otaContext.hmacCtx);
  _deviceID = _generateDeviceID();

  _otaContext.inProgress = false;
  _otaContext.currentPart = 0;
  _otaContext.totalParts = 0;
  _otaContext.receivedSize = 0;
  _otaContext.update_handle = 0;
  _otaContext.update_partition = nullptr;
  _otaContext.state = OTA_STATE_IDLE;
  _otaContext.retryCount = 0;
  _otaContext.maxRetries = MQTT_OTA_MAX_RETRIES;
  _otaContext.pendingRestart = false;
  _otaContext.restartAt = 0;
}

MQTTOTA::~MQTTOTA() {
  cleanup();
  _cleanupChunkedOTA();
}

// begin()

void MQTTOTA::begin(const String &deviceName, const String &firmwareVersion) {
  _deviceName = deviceName;
  _firmwareVersion = firmwareVersion;

  // confirm boot on every start — cancels rollback if in OTA partition
  if (_otaContext.rollbackEnabled) {
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running && running->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_0 &&
        running->subtype <= ESP_PARTITION_SUBTYPE_APP_OTA_MAX) {
      esp_ota_mark_app_valid_cancel_rollback();
      Serial.println("[MQTTOTA] Boot confirmed — rollback cancelled");
    }
  }

  Serial.println("[MQTTOTA] Initialized");
  Serial.printf("  Device  : %s\n", _deviceName.c_str());
  Serial.printf("  Version : %s\n", _firmwareVersion.c_str());
  Serial.printf("  DeviceID: %s\n", _deviceID.c_str());
  Serial.printf("  Sig req : %s\n", _requireSig ? "YES" : "no");
}

// setMQTTConfig()

void MQTTOTA::setMQTTConfig(
    std::function<void(const char *topic, const String &message)> publishFunc,
    std::function<bool()> isConnectedFunc, const String &otaTopic) {
  _publishMQTT = publishFunc;
  _isMQTTConnected = isConnectedFunc;
  _otaTopic = otaTopic;
  Serial.printf("[MQTTOTA] MQTT configured — topic: %s\n", _otaTopic.c_str());
}

// Security configuration

void MQTTOTA::setSecurityKey(const char *key) {
  if (!key)
    return;
  _hmacKeyLen = strlen(key);
  if (_hmacKeyLen > sizeof(_hmacKey))
    _hmacKeyLen = sizeof(_hmacKey);
  memcpy(_hmacKey, key, _hmacKeyLen);
  Serial.printf("[MQTTOTA] Security key set (%zu bytes)\n", _hmacKeyLen);
}

void MQTTOTA::setPublicKey(const char *pemKey) {
  if (pemKey) {
    _publicKey = String(pemKey);
    Serial.println("[MQTTOTA] ECDSA Public Key set");
  }
}

void MQTTOTA::setSecurityMode(OTASecurityMode mode) {
  _securityMode = mode;
  Serial.printf("[MQTTOTA] Security mode set to: %d\n", _securityMode);
}

void MQTTOTA::requireSignature(bool required) {
  _requireSig = required;
  _securityMode = required ? SECURITY_SHA256 : SECURITY_NONE;
  Serial.printf("[MQTTOTA] Signature requirement: %s (mapped to mode %d)\n",
                required ? "ON" : "OFF", _securityMode);
}

// Callback registration

void MQTTOTA::onProgress(MQTTOTACallback callback) {
  _progressCallback = callback;
}
void MQTTOTA::onError(MQTTOTAErrorCallback callback) {
  _errorCallback = callback;
}
void MQTTOTA::onSuccess(MQTTOTASuccessCallback callback) {
  _successCallback = callback;
}
void MQTTOTA::onStateChange(MQTTOTAStateCallback callback) {
  _stateChangeCallback = callback;
}

// handle()  — call every loop()

void MQTTOTA::handle() {

  if (!_otaContext.inProgress && _otaContext.pendingRestart) {
    unsigned long now = millis();

    if (now >= _otaContext.restartAt ||
        (now + 10000UL < _otaContext.restartAt)) {
      Serial.println("[MQTTOTA] Restarting...");
      ESP.restart();
    }
    return;
  }

  // single unified timeout check
  if (_otaContext.inProgress &&
      (millis() - _otaContext.startTime > MQTT_OTA_TIMEOUT_MS)) {
    _publishError("OTA timeout", _otaContext.firmwareVersion);
    _cleanupChunkedOTA();
    _setState(OTA_STATE_ERROR);
    Serial.println("[MQTTOTA] OTA timeout — aborted");
  }
}

// processMessage()

void MQTTOTA::processMessage(const String &topic, const String &message) {
  if (topic != _otaTopic)
    return;

  if (isUpdateInProgress()) {
    Serial.println("[MQTTOTA] OTA in progress — ignoring new message");
    return;
  }

  if (ESP.getFreeHeap() < MQTT_OTA_MIN_MEMORY) {
    Serial.printf("[MQTTOTA] Insufficient memory (%u free, need %d)\n",
                  ESP.getFreeHeap(), MQTT_OTA_MIN_MEMORY);
    _publishError("Insufficient memory");
    return;
  }

  Serial.println("[MQTTOTA] Processing OTA message...");

  if (_chunkedOTAEnabled) {
    _processOTAChunk(message);
  } else {
    _processOTAMessage(message);
  }
}

// processOTAMessage()  full firmware mode

void MQTTOTA::_processOTAMessage(const String &message) {

  StaticJsonDocument<MQTT_OTA_JSON_SIZE> doc;
  DeserializationError error = deserializeJson(doc, message);

  if (error) {
    Serial.printf("[MQTTOTA] JSON parse error: %s\n", error.c_str());
    return;
  }

  if (!doc.containsKey("EventType") ||
      doc["EventType"] != "UpdateFirmwareDevice") {
    return;
  }

  if (!doc.containsKey("Details")) {
    Serial.println("[MQTTOTA] Missing 'Details' in message");
    return;
  }

  JsonObjectConst details = doc["Details"];
  String firmwareVersion = details["FirmwareVersion"] | "";
  String base64Data = details["Base64"] | "";

  if (firmwareVersion.isEmpty() || base64Data.isEmpty()) {
    Serial.println("[MQTTOTA] Incomplete OTA data");
    return;
  }

  if (!_validateFirmwareData(base64Data))
    return;

  _setState(OTA_STATE_RECEIVING);

  Serial.printf("[MQTTOTA] Starting full OTA — version: %s, payload: %d B\n",
                firmwareVersion.c_str(), base64Data.length());

  _otaContext.inProgress = true;
  _otaContext.startTime = millis();
  _otaContext.firmwareVersion = firmwareVersion;

  _publishProgress(10, firmwareVersion);
  _setState(OTA_STATE_DECODING);

  if (performUpdate(base64Data, firmwareVersion)) {
    _setState(OTA_STATE_SUCCESS);
    _publishSuccess(firmwareVersion);
    Serial.println("[MQTTOTA] Full OTA complete");

    _otaContext.inProgress = false;
    _stats.endTime = millis();

    if (_autoReset) {
      _otaContext.pendingRestart = true;
      _otaContext.restartAt = millis() + MQTT_OTA_RESTART_DELAY_MS;
      Serial.printf("[MQTTOTA] Restarting in %lu ms (call handle() in loop)\n",
                    MQTT_OTA_RESTART_DELAY_MS);
    }
  } else {
    _otaContext.inProgress = false;
    _setState(OTA_STATE_ERROR);
    cleanup();
  }
}

// processOTAChunk()  chunked mode

void MQTTOTA::_processOTAChunk(const String &message) {

  StaticJsonDocument<4096> doc;
  DeserializationError error = deserializeJson(doc, message);

  if (error) {
    Serial.printf("[MQTTOTA] JSON chunk parse error: %s\n", error.c_str());
    return;
  }

  if (!doc.containsKey("EventType") ||
      doc["EventType"] != "UpdateFirmwareDevice") {
    return;
  }

  if (!doc.containsKey("Details")) {
    Serial.println("[MQTTOTA] Missing 'Details' in chunk message");
    return;
  }

  JsonObjectConst details = doc["Details"];
  OTAChunkData chunk;

  chunk.firmwareVersion = details["FirmwareVersion"].as<String>();
  chunk.base64Part = details["Base64Part"].as<String>();
  chunk.partIndex = details["PartIndex"] | 0;
  chunk.totalParts = details["TotalParts"] | 0;
  chunk.isError = details["IsError"] | false;
  chunk.errorMessage = details["ErrorMessage"] | "";
  chunk.checksum = details["checksum"] | details["sha256"] | "";
  chunk.hmacSig = details["hmac_sig"] | "";
  chunk.ecdsaSig = details["ecdsa_sig"] | "";

  if (chunk.isError) {
    Serial.printf("[MQTTOTA] Sender error: %s\n", chunk.errorMessage.c_str());
    _publishError(chunk.errorMessage, chunk.firmwareVersion);
    _cleanupChunkedOTA();
    return;
  }

  if (chunk.base64Part.isEmpty() || chunk.firmwareVersion.isEmpty()) {
    _publishError("Incomplete chunk data", chunk.firmwareVersion);
    _cleanupChunkedOTA();
    return;
  }

  // First chunk  initialise session
  if (chunk.partIndex == 1) {
    if (_otaContext.inProgress) {
      Serial.println(
          "[MQTTOTA] OTA already in progress — ignoring new chunk 1");
      return;
    }
    if (!_startChunkedOTA(chunk))
      return;
  }

  if (!_otaContext.inProgress ||
      chunk.partIndex != _otaContext.currentPart + 1) {
    Serial.printf("[MQTTOTA] Out-of-sequence chunk. Expected %d, got %d\n",
                  _otaContext.currentPart + 1, chunk.partIndex);
    _publishError("Chunk out of sequence", chunk.firmwareVersion);
    _cleanupChunkedOTA();
    return;
  }

  // Process data
  if (!_processChunkData(chunk)) {
    _cleanupChunkedOTA();
    return;
  }

  _otaContext.currentPart = chunk.partIndex;
  int progress = (chunk.partIndex * 100) / chunk.totalParts;
  _currentProgress = progress;

  _publishProgress(progress, chunk.firmwareVersion);
  Serial.printf("[MQTTOTA] Chunk %d/%d — %d%% — heap=%u\n", chunk.partIndex,
                chunk.totalParts, progress, ESP.getFreeHeap());

  // Last chunk
  if (chunk.partIndex == chunk.totalParts) {
    _completeChunkedOTA(chunk);
  }
}

// startChunkedOTA()

bool MQTTOTA::_startChunkedOTA(const OTAChunkData &chunk) {
  Serial.printf("[MQTTOTA] Starting chunked OTA — version=%s parts=%d\n",
                chunk.firmwareVersion.c_str(), chunk.totalParts);

  _setState(OTA_STATE_RECEIVING);

  _otaContext.update_partition = esp_ota_get_next_update_partition(nullptr);
  if (!_otaContext.update_partition) {
    _publishError("No OTA partition found", chunk.firmwareVersion);
    _setState(OTA_STATE_ERROR);
    return false;
  }

  esp_err_t err =
      esp_ota_begin(_otaContext.update_partition, OTA_WITH_SEQUENTIAL_WRITES,
                    &_otaContext.update_handle);
  if (err != ESP_OK) {
    String msg = String("esp_ota_begin failed: ") + esp_err_to_name(err);
    _publishError(msg, chunk.firmwareVersion);
    _setState(OTA_STATE_ERROR);
    return false;
  }

  _otaContext.inProgress = true;
  _otaContext.firmwareVersion = chunk.firmwareVersion;
  _otaContext.expectedSha256 = chunk.checksum;
  _otaContext.expectedHmac = chunk.hmacSig;
  _otaContext.expectedEcdsa = chunk.ecdsaSig;
  _otaContext.currentPart = 0;
  _otaContext.totalParts = chunk.totalParts;
  _otaContext.sha256_finished = false;
  _otaContext.startTime = millis();
  _otaContext.receivedSize = 0;
  _otaContext.retryCount = 0;
  _otaContext.pendingRestart = false;

  // Init incremental SHA-256
  mbedtls_sha256_init(&_otaContext.sha256_ctx);
  mbedtls_sha256_starts(&_otaContext.sha256_ctx, 0); // 0 = SHA-256 (not SHA-224)
  _otaContext.sha256_active = true;

  _otaContext.hmac_active = false;
  if (_hmacKeyLen > 0) {
    const mbedtls_md_info_t *mdInfo =
        mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    mbedtls_md_init(&_otaContext.hmacCtx);
    if (mbedtls_md_setup(&_otaContext.hmacCtx, mdInfo, 1) == 0) { // 1 = HMAC mode
      mbedtls_md_hmac_starts(&_otaContext.hmacCtx, _hmacKey, _hmacKeyLen);
      _otaContext.hmac_active = true;
    } else {
      Serial.println("[MQTTOTA] WARN: HMAC context setup failed");
      mbedtls_md_free(&_otaContext.hmacCtx);
    }
  }

  _stats.startTime = millis();
  _stats.chunkCount = 0;
  _stats.receivedBytes = 0;
  _stats.totalBytes = 0;
  _stats.errorCount = 0;

  _publishProgress(0, chunk.firmwareVersion);
  return true;
}

// processChunkData()

bool MQTTOTA::_processChunkData(const OTAChunkData &chunk) {
  if (!_otaContext.inProgress || _otaContext.update_handle == 0) {
    _publishError("OTA handle invalid", chunk.firmwareVersion);
    return false;
  }

  _setState(OTA_STATE_DECODING);

  String decodedData = base64Decode(chunk.base64Part);
  if (decodedData.length() == 0) {
    _publishError("Base64 decode error", chunk.firmwareVersion);
    return false;
  }

  // Verify image header on first chunk
  if (chunk.partIndex == 1) {
    _setState(OTA_STATE_VALIDATING);
    const uint8_t *raw = (const uint8_t *)decodedData.c_str();
    if (!_verifyImageIntegrity(raw, decodedData.length())) {
      _publishError("Invalid firmware image header", chunk.firmwareVersion);
      return false;
    }
    if (!_processImageHeader(raw, decodedData.length())) {
      _publishError("Firmware header size too small", chunk.firmwareVersion);
      return false;
    }
    Serial.println("[MQTTOTA] Image header OK");
  }

  if (_otaContext.sha256_active) {
    mbedtls_sha256_update(&_otaContext.sha256_ctx, (const uint8_t*)decodedData.c_str(), decodedData.length());
  }

  if (_otaContext.hmac_active) {
    mbedtls_md_hmac_update(&_otaContext.hmacCtx, (const uint8_t*)decodedData.c_str(), decodedData.length());
  }

  _setState(OTA_STATE_WRITING);

  esp_err_t err =
      esp_ota_write(_otaContext.update_handle,
                    (const void *)decodedData.c_str(), decodedData.length());

  if (err != ESP_OK) {
    String msg = String("esp_ota_write failed: ") + esp_err_to_name(err);
    _publishError(msg, chunk.firmwareVersion);
    return false;
  }

  _otaContext.receivedSize += decodedData.length();

  // Update stats
  unsigned long elapsed = millis() - _otaContext.startTime;
  if (elapsed > 0) {
    _stats.avgSpeedBps = (_otaContext.receivedSize * 1000.0f) / (float)elapsed;
  }
  _stats.receivedBytes = _otaContext.receivedSize;
  _stats.chunkCount++;

  Serial.printf("[MQTTOTA] Chunk %d: %d B written, total %zu B\n",
                chunk.partIndex, decodedData.length(),
                _otaContext.receivedSize);
  return true;
}

// completeChunkedOTA()

void MQTTOTA::_completeChunkedOTA(const OTAChunkData &chunk) {
  Serial.printf("[MQTTOTA] Completing OTA — %zu bytes written\n",
                _otaContext.receivedSize);

  _setState(OTA_STATE_COMPLETING);

  if (_otaContext.receivedSize < 1024) {
    _publishError("Firmware too small", chunk.firmwareVersion);
    _cleanupChunkedOTA();
    return;
  }

  _publishProgress(90, chunk.firmwareVersion);

  _setState(OTA_STATE_VALIDATING);

  _finishSha256Digest(); // finalize the digest once

  if (_securityMode == SECURITY_ECDSA_SHA256) {
    if (_otaContext.expectedEcdsa.isEmpty()) {
      _publishError("No ecdsa_sig provided for SECURITY_ECDSA_SHA256", chunk.firmwareVersion);
      _cleanupChunkedOTA();
      _setState(OTA_STATE_ERROR);
      return;
    }
    if (!_verifyEcdsaFinal(_otaContext.expectedEcdsa)) {
      _publishError("ECDSA signature mismatch — firmware rejected", chunk.firmwareVersion);
      _cleanupChunkedOTA();
      _setState(OTA_STATE_ERROR);
      return;
    }
    Serial.println("[MQTTOTA] ECDSA Signature OK");
  } else if (_securityMode == SECURITY_HMAC_SHA256) {
    if (_otaContext.expectedHmac.isEmpty() || _hmacKeyLen == 0) {
      _publishError("No hmac_sig or key provided for SECURITY_HMAC_SHA256", chunk.firmwareVersion);
      _cleanupChunkedOTA();
      _setState(OTA_STATE_ERROR);
      return;
    }
    if (!_verifyHmacFinal(_otaContext.expectedHmac)) {
      _publishError("HMAC mismatch — firmware rejected", chunk.firmwareVersion);
      _cleanupChunkedOTA();
      _setState(OTA_STATE_ERROR);
      return;
    }
    Serial.println("[MQTTOTA] HMAC OK");
  } else if (_securityMode == SECURITY_SHA256) {
    if (_otaContext.expectedSha256.isEmpty()) {
      _publishError("No sha256 provided for SECURITY_SHA256", chunk.firmwareVersion);
      _cleanupChunkedOTA();
      _setState(OTA_STATE_ERROR);
      return;
    }
    if (!_verifySha256Final(_otaContext.expectedSha256)) {
      _publishError("SHA-256 mismatch — firmware rejected", chunk.firmwareVersion);
      _cleanupChunkedOTA();
      _setState(OTA_STATE_ERROR);
      return;
    }
    Serial.println("[MQTTOTA] SHA-256 OK");
  } else {
    // SECURITY_NONE: check SHA-256 if provided, otherwise accept
    if (!_otaContext.expectedSha256.isEmpty()) {
      if (!_verifySha256Final(_otaContext.expectedSha256)) {
        _publishError("SHA-256 mismatch (dev mode) — firmware rejected", chunk.firmwareVersion);
        _cleanupChunkedOTA();
        _setState(OTA_STATE_ERROR);
        return;
      }
      Serial.println("[MQTTOTA] SHA-256 OK (dev mode)");
    } else {
      Serial.println("[MQTTOTA] No signature required (SECURITY_NONE)");
    }
  }

  esp_err_t err = esp_ota_end(_otaContext.update_handle);
  _otaContext.update_handle = 0;

  if (err != ESP_OK) {
    String msg = String("esp_ota_end failed: ") + esp_err_to_name(err);
    if (err == ESP_ERR_OTA_VALIDATE_FAILED)
      msg += " (image validation)";
    _publishError(msg, chunk.firmwareVersion);
    _cleanupChunkedOTA();
    _setState(OTA_STATE_ERROR);
    return;
  }

  _publishProgress(95, chunk.firmwareVersion);

  err = esp_ota_set_boot_partition(_otaContext.update_partition);
  if (err != ESP_OK) {
    String msg =
        String("esp_ota_set_boot_partition failed: ") + esp_err_to_name(err);
    _publishError(msg, chunk.firmwareVersion);
    _cleanupChunkedOTA();
    _setState(OTA_STATE_ERROR);
    return;
  }

  _publishProgress(100, chunk.firmwareVersion);

  _setState(OTA_STATE_SUCCESS);
  _publishSuccess(chunk.firmwareVersion);

  _stats.endTime = millis();
  _stats.lastState = OTA_STATE_SUCCESS;

  Serial.printf("[MQTTOTA] OTA SUCCESS — %s in %.1f s\n",
                chunk.firmwareVersion.c_str(),
                (millis() - _otaContext.startTime) / 1000.0f);

  _otaContext.inProgress = false;

  if (_otaContext.sha256_active) {
    mbedtls_sha256_free(&_otaContext.sha256_ctx);
    _otaContext.sha256_active = false;
    _otaContext.sha256_finished = false;
  }

  if (_otaContext.hmac_active) {
    mbedtls_md_free(&_otaContext.hmacCtx);
    _otaContext.hmac_active = false;
  }

  // non-blocking restart
  if (_autoReset) {
    _otaContext.pendingRestart = true;
    _otaContext.restartAt = millis() + MQTT_OTA_RESTART_DELAY_MS;
    Serial.printf("[MQTTOTA] Restarting in %lu ms (call handle() in loop)\n",
                  MQTT_OTA_RESTART_DELAY_MS);
  }
}

// cleanupChunkedOTA()

void MQTTOTA::_cleanupChunkedOTA() {
  if (_otaContext.update_handle != 0) {
    if (_otaContext.inProgress) {
      esp_ota_abort(_otaContext.update_handle);
      Serial.println("[MQTTOTA] OTA aborted");
    }
    _otaContext.update_handle = 0;
  }

  if (_otaContext.sha256_active) {
    mbedtls_sha256_free(&_otaContext.sha256_ctx);
    _otaContext.sha256_active = false;
    _otaContext.sha256_finished = false;
  }

  if (_otaContext.hmac_active) {
    mbedtls_md_free(&_otaContext.hmacCtx);
    _otaContext.hmac_active = false;
  }

  _otaContext.inProgress = false;
  _otaContext.currentPart = 0;
  _otaContext.totalParts = 0;
  _otaContext.receivedSize = 0;
  _otaContext.startTime = 0;
  _otaContext.update_partition = nullptr;
  _otaContext.retryCount = 0;
  _otaContext.pendingRestart = false;
  _currentProgress = 0;

  _setState(OTA_STATE_IDLE);
}

// performUpdate() / performOTAUpdateESPIDF()

bool MQTTOTA::performUpdate(const String &base64Data,
                            const String &firmwareVersion) {
  return _performOTAUpdateESPIDF(base64Data, firmwareVersion);
}

bool MQTTOTA::_performOTAUpdateESPIDF(const String &base64Data,
                                      const String &firmwareVersion) {
  Serial.println("[MQTTOTA] Starting full OTA with ESP-IDF...");

  if (ESP.getFreeHeap() < 50000) {
    _publishError("Insufficient memory for full OTA", firmwareVersion);
    return false;
  }

  _setState(OTA_STATE_DECODING);

  String decodedData = base64Decode(base64Data);
  if (decodedData.length() == 0) {
    _publishError("Base64 decode failed", firmwareVersion);
    return false;
  }

  Serial.printf("[MQTTOTA] Decoded: %d B, free heap: %u\n",
                decodedData.length(), ESP.getFreeHeap());

  const esp_partition_t *update_partition =
      esp_ota_get_next_update_partition(nullptr);
  if (!update_partition) {
    _publishError("No OTA partition found", firmwareVersion);
    return false;
  }

  esp_ota_handle_t update_handle;
  esp_err_t err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES,
                                &update_handle);
  if (err != ESP_OK) {
    _publishError(String("esp_ota_begin failed: ") + esp_err_to_name(err),
                  firmwareVersion);
    return false;
  }

  _publishProgress(25, firmwareVersion);

  size_t total = decodedData.length();
  size_t written = 0;
  size_t chunk_size = _chunkSize;

  for (size_t i = 0; i < total; i += chunk_size) {
    size_t csz = min(chunk_size, total - i);

    if (i == 0) {
      _setState(OTA_STATE_VALIDATING);
      const uint8_t *raw = (const uint8_t *)decodedData.c_str();
      if (!_processImageHeader(raw, csz)) {
        esp_ota_abort(update_handle);
        _publishError("Invalid firmware image header", firmwareVersion);
        return false;
      }
      if (!_verifyImageIntegrity(raw, csz)) {
        esp_ota_abort(update_handle);
        _publishError("Firmware magic number invalid", firmwareVersion);
        return false;
      }
    }

    _setState(OTA_STATE_WRITING);

    err = esp_ota_write(update_handle, (const void *)(decodedData.c_str() + i),
                        csz);
    if (err != ESP_OK) {
      esp_ota_abort(update_handle);
      _publishError(String("esp_ota_write failed: ") + esp_err_to_name(err),
                    firmwareVersion);
      return false;
    }

    written += csz;

    int progress = 25 + (int)(written * 50 / total);
    if (progress > 75)
      progress = 75;
    if ((written * 100 / total) % 10 == 0)
      _publishProgress(progress, firmwareVersion);

    Serial.printf("[MQTTOTA] Written %zu / %zu B (%.1f%%)\n", written, total,
                  written * 100.0 / total);
  }

  _publishProgress(75, firmwareVersion);
  _setState(OTA_STATE_COMPLETING);

  err = esp_ota_end(update_handle);
  if (err != ESP_OK) {
    _publishError(String("esp_ota_end failed: ") + esp_err_to_name(err),
                  firmwareVersion);
    return false;
  }

  err = esp_ota_set_boot_partition(update_partition);
  if (err != ESP_OK) {
    _publishError(String("esp_ota_set_boot_partition failed: ") +
                      esp_err_to_name(err),
                  firmwareVersion);
    return false;
  }

  _publishProgress(100, firmwareVersion);
  Serial.println("[MQTTOTA] Full OTA complete");
  return true;
}

// validateFirmwareData()

bool MQTTOTA::_validateFirmwareData(const String &base64Data) {
  if (base64Data.isEmpty()) {
    _publishError("Empty firmware data");
    return false;
  }
  if (base64Data.length() < 100) {
    _publishError("Firmware data too short");
    return false;
  }
  for (unsigned int i = 0; i < base64Data.length(); i++) {
    char c = base64Data.charAt(i);
    if (!isalnum(c) && c != '+' && c != '/' && c != '=' && c != '\n' &&
        c != '\r') {
      _publishError("Invalid Base64 character");
      return false;
    }
  }
  return true;
}

// Publishing helpers  (StaticJsonDocument)

void MQTTOTA::_publishError(const String &errorMessage,
                            const String &firmwareVersion) {
  _stats.errorCount++;

  if (_errorCallback) {
    _errorCallback(errorMessage, firmwareVersion.isEmpty() ? _firmwareVersion
                                                           : firmwareVersion);
  }

  Serial.printf("[MQTTOTA] ERROR: %s\n", errorMessage.c_str());

  if (_publishMQTT && _isMQTTConnected && _isMQTTConnected()) {
    StaticJsonDocument<256> doc;
    doc["device"] = _deviceID;
    doc["version"] =
        firmwareVersion.isEmpty() ? _firmwareVersion : firmwareVersion;
    doc["error"] = errorMessage;
    doc["timestamp"] = millis();

    String output;
    serializeJson(doc, output);
    _publishMQTT("ota/error", output);
  }
}

void MQTTOTA::_publishSuccess(const String &firmwareVersion) {
  if (_successCallback)
    _successCallback(firmwareVersion);

  if (_publishMQTT && _isMQTTConnected && _isMQTTConnected()) {
    StaticJsonDocument<256> doc;
    doc["device"] = _deviceID;
    doc["version"] = firmwareVersion;
    doc["success"] = true;
    doc["timestamp"] = millis();

    String output;
    serializeJson(doc, output);
    _publishMQTT("ota/success", output);
  }

  Serial.printf("[MQTTOTA] SUCCESS — version: %s\n", firmwareVersion.c_str());
}

void MQTTOTA::_publishProgress(int progress, const String &firmwareVersion) {
  _currentProgress = progress;

  if (_progressCallback)
    _progressCallback(progress, firmwareVersion);

  if (_publishMQTT && _isMQTTConnected && _isMQTTConnected() &&
      (progress % 10 == 0 || progress == 100)) {
    StaticJsonDocument<256> doc;
    doc["device"] = _deviceID;
    doc["version"] = firmwareVersion;
    doc["progress"] = progress;
    doc["timestamp"] = millis();

    String output;
    serializeJson(doc, output);
    _publishMQTT("ota/progress", output);
  }

  Serial.printf("[MQTTOTA] Progress: %d%%\n", progress);
}

void MQTTOTA::_publishStateChange(OTAState state) {
  if (_stateChangeCallback)
    _stateChangeCallback(static_cast<uint8_t>(state));

  if (_publishMQTT && _isMQTTConnected && _isMQTTConnected()) {
    StaticJsonDocument<256> doc;
    doc["device"] = _deviceID;
    doc["state"] = static_cast<uint8_t>(state);
    doc["state_name"] = _getStateName(state);
    doc["timestamp"] = millis();

    String output;
    serializeJson(doc, output);
    _publishMQTT("ota/state", output);
  }

  Serial.printf("[MQTTOTA] State → %s\n", _getStateName(state).c_str());
}

// cleanup() / abortUpdate()

void MQTTOTA::cleanup() {
  _otaContext.inProgress = false;
  _currentProgress = 0;
  _otaContext.startTime = 0;
  _otaContext.firmwareVersion = "";
  _otaContext.pendingRestart = false;
}

void MQTTOTA::abortUpdate() {
  if (isUpdateInProgress()) {
    _publishError("Aborted by user", _otaContext.firmwareVersion);
    _cleanupChunkedOTA();
    cleanup();
    _setState(OTA_STATE_ABORTED);
    Serial.println("[MQTTOTA] Update aborted by user");
  }
}

// SHA-256  (implementation with mbedtls)

String MQTTOTA::_calculateSHA256(const uint8_t *data, size_t length) {
  uint8_t digest[MQTT_OTA_HASH_LEN];
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0); // 0 = SHA-256 (not SHA-224)
  mbedtls_sha256_update(&ctx, data, length);
  mbedtls_sha256_finish(&ctx, digest);
  mbedtls_sha256_free(&ctx);

  char hex[MQTT_OTA_HASH_LEN * 2 + 1];
  for (int i = 0; i < MQTT_OTA_HASH_LEN; ++i) {
    snprintf(&hex[i * 2], 3, "%02x", digest[i]);
  }
  return String(hex);
}

// HMAC-SHA256

String MQTTOTA::_hmacSha256Hex(const uint8_t *data, size_t len) const {
  if (_hmacKeyLen == 0)
    return "";

  uint8_t digest[MQTT_OTA_HASH_LEN];
  const mbedtls_md_info_t *mdInfo =
      mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  mbedtls_md_hmac(mdInfo, _hmacKey, _hmacKeyLen, data, len, digest);

  char hex[MQTT_OTA_HASH_LEN * 2 + 1];
  for (int i = 0; i < MQTT_OTA_HASH_LEN; ++i) {
    snprintf(&hex[i * 2], 3, "%02x", digest[i]);
  }
  return String(hex);
}

// New public overload: raw bytes + expected hex
bool MQTTOTA::verifyFirmwareSignature(const uint8_t *data, size_t len,
                                      const String &expectedHex) {
  if (expectedHex.isEmpty()) {
    if (_requireSig) {
      Serial.println("[MQTTOTA] ERROR: signature required but not provided");
      return false;
    }
    return true;
  }
  if (_hmacKeyLen == 0) {
    Serial.println(
        "[MQTTOTA] WARNING: signature provided but no key set — skipping");
    return true;
  }

  String computed = _hmacSha256Hex(data, len);
  bool ok = computed.equalsIgnoreCase(expectedHex);

  if (!ok) {
    Serial.printf("[MQTTOTA] HMAC mismatch\n  expected: %s\n  computed: %s\n",
                  expectedHex.c_str(), computed.c_str());
  } else {
    Serial.println("[MQTTOTA] HMAC OK");
  }
  return ok;
}

// Legacy overload: kept for backward compatibility (does nothing useful without
// data)
bool MQTTOTA::verifyFirmwareSignature(const String &signature) {
  if (signature.isEmpty()) {
    if (_requireSig) {
      Serial.println("[MQTTOTA] ERROR: signature required but not provided");
      return false;
    }
    Serial.println("[MQTTOTA] WARNING: no signature provided");
    return true;
  }
  // Without raw data we cannot compute HMAC — accept if a key is not set
  if (_hmacKeyLen == 0)
    return true;
  Serial.println("[MQTTOTA] WARNING: use verifyFirmwareSignature(data, len, "
                 "hex) for HMAC check");
  return true;
}

// getFreeOTASpace()

size_t MQTTOTA::getFreeOTASpace() {
  const esp_partition_t *part = esp_ota_get_next_update_partition(nullptr);
  if (!part)
    return 0;
  return (size_t)part->size;
}

// processImageHeader() / verifyImageIntegrity()

bool MQTTOTA::_processImageHeader(const uint8_t *data, size_t data_len) {
  const size_t minLen = sizeof(esp_image_header_t) +
                        sizeof(esp_image_segment_header_t) +
                        sizeof(esp_app_desc_t);
  if (data_len < minLen) {
    Serial.printf("[MQTTOTA] First chunk too small: %zu < %zu\n", data_len,
                  minLen);
    return false;
  }

  esp_app_desc_t appDesc;
  memcpy(&appDesc,
         data + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t),
         sizeof(esp_app_desc_t));
  Serial.printf("[MQTTOTA] Firmware version: %s  IDF: %s\n", appDesc.version,
                appDesc.idf_ver);
  return true;
}

bool MQTTOTA::_verifyImageIntegrity(const uint8_t *data, size_t length) {
  if (length < sizeof(esp_image_header_t))
    return false;

  const esp_image_header_t *header = (const esp_image_header_t *)data;

  if (header->magic != ESP_IMAGE_HEADER_MAGIC) {
    Serial.printf("[MQTTOTA] Bad magic: 0x%02X (expected 0x%02X)\n",
                  header->magic, ESP_IMAGE_HEADER_MAGIC);
    return false;
  }
  if (header->segment_count == 0) {
    Serial.println("[MQTTOTA] Zero segments in image");
    return false;
  }
  return true;
}

// Partition validation

bool MQTTOTA::_validatePartitionWrite() {
  if (!_otaContext.update_partition)
    return false;

  if (_otaContext.update_partition->type != ESP_PARTITION_TYPE_APP) {
    Serial.println("[MQTTOTA] Partition type is not APP");
    return false;
  }
  if (_otaContext.update_partition->size < 65536) {
    Serial.printf("[MQTTOTA] Partition too small: %u B\n",
                  _otaContext.update_partition->size);
    return false;
  }
  Serial.printf("[MQTTOTA] Partition OK: %s @ 0x%08X (%u B)\n",
                _otaContext.update_partition->label,
                _otaContext.update_partition->address,
                _otaContext.update_partition->size);
  return true;
}

// Version / rollback helpers

bool MQTTOTA::_checkFirmwareVersion(const String &newVersion) {
  if (!_otaContext.versionCheckEnabled)
    return true;
  return (newVersion != _firmwareVersion && !newVersion.isEmpty());
}

bool MQTTOTA::_checkRollbackProtection() {
  if (!_otaContext.rollbackEnabled)
    return true;
  const esp_partition_t *running = esp_ota_get_running_partition();
  return (running != nullptr);
}

// State management

void MQTTOTA::_setState(OTAState state) {
  if (_otaContext.state == state)
    return;
  _otaContext.state = state;
  _stats.lastState = state;
  _publishStateChange(state);
}

// Statistics

void MQTTOTA::_updateStatistics(size_t bytesReceived, bool isError) {
  if (_stats.startTime == 0 && bytesReceived > 0) {
    _stats.startTime = millis();
  }
  if (bytesReceived > 0) {
    _stats.totalBytes += bytesReceived;
    _stats.receivedBytes += bytesReceived;
    _stats.chunkCount++;

    unsigned long elapsed = millis() - _stats.startTime;
    if (elapsed > 1000) {
      _stats.averageSpeed = (_stats.receivedBytes * 1000.0f) / elapsed;
    }
  }
  if (isError)
    _stats.errorCount++;
  _stats.lastState = _otaContext.state;
}

// Memory utilities

bool MQTTOTA::checkMemory(size_t requiredBytes) {
  size_t freeHeap = ESP.getFreeHeap();
  bool ok = freeHeap >= (requiredBytes + MQTT_OTA_MIN_MEMORY);
  if (!ok) {
    Serial.printf("[MQTTOTA] Low memory: %u free, need %zu\n", freeHeap,
                  requiredBytes + MQTT_OTA_MIN_MEMORY);
  }
  return ok;
}

size_t MQTTOTA::getFreeHeap() { return ESP.getFreeHeap(); }

void MQTTOTA::logMemoryStatus() {
  Serial.printf("[MQTTOTA] Heap — free=%u  minFree=%u  maxAlloc=%u\n",
                ESP.getFreeHeap(), ESP.getMinFreeHeap(), ESP.getMaxAllocHeap());
}

// Diagnostics

void MQTTOTA::printDiagnostics() {
  Serial.println("=== MQTTOTA Diagnostics ===");
  Serial.printf("  DeviceID : %s\n", _deviceID.c_str());
  Serial.printf("  Device   : %s\n", _deviceName.c_str());
  Serial.printf("  Version  : %s\n", _firmwareVersion.c_str());
  Serial.printf("  State    : %s\n", _getStateName(_otaContext.state).c_str());
  Serial.printf("  Progress : %d%%\n", _currentProgress);
  Serial.printf("  Received : %zu B\n", _otaContext.receivedSize);
  Serial.printf("  Sig req  : %s\n", _requireSig ? "yes" : "no");
  Serial.printf("  Key set  : %s\n", _hmacKeyLen > 0 ? "yes" : "no");

  logMemoryStatus();

  const esp_partition_t *running = esp_ota_get_running_partition();
  if (running) {
    Serial.printf("  Running  : %s @ 0x%08X\n", running->label,
                  running->address);
  }
  if (_otaContext.update_partition) {
    Serial.printf("  OTA Part : %s @ 0x%08X (%u B)\n",
                  _otaContext.update_partition->label,
                  _otaContext.update_partition->address,
                  _otaContext.update_partition->size);
  }

  Serial.printf("  Stats — chunks=%d errors=%d speed=%.1f KB/s\n",
                _stats.chunkCount, _stats.errorCount,
                _stats.averageSpeed / 1024.0f);
  Serial.println("===========================");
}

String MQTTOTA::getBootPartitionInfo() {
  const esp_partition_t *boot = esp_ota_get_boot_partition();
  if (!boot)
    return "unknown";
  char buf[256];
  snprintf(buf, sizeof(buf), "label=%s type=%d subtype=%d addr=0x%08X size=%u",
           boot->label, boot->type, boot->subtype, boot->address, boot->size);
  return String(buf);
}

// Miscellaneous helpers

void MQTTOTA::_printSHA256(const uint8_t *image_hash, const char *label) {
  char hash_print[MQTT_OTA_HASH_LEN * 2 + 1];
  hash_print[MQTT_OTA_HASH_LEN * 2] = 0;
  for (int i = 0; i < MQTT_OTA_HASH_LEN; ++i) {
    sprintf(&hash_print[i * 2], "%02x", image_hash[i]);
  }
  Serial.printf("%s: %s\n", label, hash_print);
}

// Finalize security digests and verify

void MQTTOTA::_finishSha256Digest() {
  if (!_otaContext.sha256_active || _otaContext.sha256_finished) return;
  mbedtls_sha256_finish(&_otaContext.sha256_ctx, _otaContext.finalDigest);
  _otaContext.sha256_finished = true;
}

bool MQTTOTA::_verifySha256Final(const String &expectedHex) {
  if (!_otaContext.sha256_active || !_otaContext.sha256_finished)
    return false;

  char hex[MQTT_OTA_HASH_LEN * 2 + 1];
  for (int i = 0; i < MQTT_OTA_HASH_LEN; ++i) {
    snprintf(&hex[i * 2], 3, "%02x", _otaContext.finalDigest[i]);
  }

  bool ok = (expectedHex.equalsIgnoreCase(String(hex)));
  if (!ok) {
    Serial.printf(
        "[MQTTOTA] SHA-256 MISMATCH\n  expected: %s\n  computed: %s\n",
        expectedHex.c_str(), hex);
  }
  return ok;
}

bool MQTTOTA::_verifyHmacFinal(const String &expectedHex) {
  if (!_otaContext.hmac_active)
    return false;

  uint8_t digest[MQTT_OTA_HASH_LEN];
  mbedtls_md_hmac_finish(&_otaContext.hmacCtx, digest);

  char hex[MQTT_OTA_HASH_LEN * 2 + 1];
  for (int i = 0; i < MQTT_OTA_HASH_LEN; ++i) {
    snprintf(&hex[i * 2], 3, "%02x", digest[i]);
  }

  bool ok = expectedHex.equalsIgnoreCase(String(hex));
  if (!ok) {
    Serial.printf("[MQTTOTA] HMAC MISMATCH\n  expected: %s\n  computed: %s\n",
                  expectedHex.c_str(), hex);
  }
  return ok;
}

bool MQTTOTA::_verifyEcdsaFinal(const String &expectedSigBase64) {
  if (!_otaContext.sha256_active || !_otaContext.sha256_finished || _publicKey.isEmpty()) {
    return false;
  }

  mbedtls_pk_context pk;
  mbedtls_pk_init(&pk);

  // Parse public key
  int ret = mbedtls_pk_parse_public_key(&pk, (const unsigned char *)_publicKey.c_str(), _publicKey.length() + 1);
  if (ret != 0) {
    Serial.printf("[MQTTOTA] mbedtls_pk_parse_public_key failed: -0x%04x\n", -ret);
    mbedtls_pk_free(&pk);
    return false;
  }

  // Decode Base64 signature
  size_t sigLen = calculateBase64DecodedSize(expectedSigBase64);
  uint8_t* sigBuf = (uint8_t*)malloc(sigLen + 4);
  if (!sigBuf) {
    mbedtls_pk_free(&pk);
    return false;
  }
  
  String decodedSig = base64Decode(expectedSigBase64);
  if (decodedSig.isEmpty()) {
    free(sigBuf);
    mbedtls_pk_free(&pk);
    return false;
  }
  memcpy(sigBuf, decodedSig.c_str(), decodedSig.length());

  // Verify signature
  ret = mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, _otaContext.finalDigest, sizeof(_otaContext.finalDigest), sigBuf, decodedSig.length());
  free(sigBuf);
  mbedtls_pk_free(&pk);

  if (ret != 0) {
    Serial.printf("[MQTTOTA] mbedtls_pk_verify failed: -0x%04x\n", -ret);
    return false;
  }

  return true;
}

String MQTTOTA::_generateDeviceID() {
  uint64_t mac = ESP.getEfuseMac();
  char buf[17];
  snprintf(buf, sizeof(buf), "%04X%08X", (uint16_t)(mac >> 32), (uint32_t)mac);
  return String(buf);
}

size_t MQTTOTA::calculateBase64DecodedSize(const String &encoded) {
  size_t len = encoded.length();
  size_t padding = 0;
  if (len > 0 && encoded.charAt(len - 1) == '=')
    padding++;
  if (len > 1 && encoded.charAt(len - 2) == '=')
    padding++;
  return (len * 3) / 4 - padding;
}

bool MQTTOTA::checkFirmwareCompatibility(const String &newVersion) {
  if (newVersion.isEmpty())
    return false;
  int dots = 0;
  for (size_t i = 0; i < newVersion.length(); i++) {
    char c = newVersion.charAt(i);
    if (c == '.')
      dots++;
    else if (!isdigit(c) && c != '-' && c != '+')
      return false;
  }
  return (dots >= 1 && dots <= 2);
}

bool MQTTOTA::_validateChecksum(const String &data, const String &checksum) {
  if (checksum.isEmpty())
    return true;
  // Compute SHA-256 of the data and compare
  String computed =
      _calculateSHA256((const uint8_t *)data.c_str(), data.length());
  bool ok = computed.equalsIgnoreCase(checksum);
  if (!ok) {
    Serial.printf(
        "[MQTTOTA] Checksum mismatch\n  expected: %s\n  computed: %s\n",
        checksum.c_str(), computed.c_str());
  }
  return ok;
}

void MQTTOTA::_handleChunkError(const OTAChunkData &chunk,
                                const String &error) {
  Serial.printf("[MQTTOTA] Chunk %d error: %s\n", chunk.partIndex,
                error.c_str());

  _otaContext.retryCount++;
  if (_otaContext.retryCount <= _otaContext.maxRetries) {
    Serial.printf("[MQTTOTA] Retry %d/%d for chunk %d\n",
                  _otaContext.retryCount, _otaContext.maxRetries,
                  chunk.partIndex);
  } else {
    _publishError("Max retries exceeded: " + error, chunk.firmwareVersion);
    _cleanupChunkedOTA();
  }
}

String MQTTOTA::_getStateName(OTAState state) {
  switch (state) {
  case OTA_STATE_IDLE:
    return "IDLE";
  case OTA_STATE_RECEIVING:
    return "RECEIVING";
  case OTA_STATE_DECODING:
    return "DECODING";
  case OTA_STATE_VALIDATING:
    return "VALIDATING";
  case OTA_STATE_WRITING:
    return "WRITING";
  case OTA_STATE_COMPLETING:
    return "COMPLETING";
  case OTA_STATE_SUCCESS:
    return "SUCCESS";
  case OTA_STATE_ERROR:
    return "ERROR";
  case OTA_STATE_ABORTED:
    return "ABORTED";
  default:
    return "UNKNOWN";
  }
}

void MQTTOTA::setPartitionName(const String &partitionName) {
  _otaContext.partitionName = partitionName;
}

OTAState MQTTOTA::getCurrentState() { return _otaContext.state; }

OTAStatistics MQTTOTA::getStatistics() { return _stats; }

void MQTTOTA::enableRollbackProtection(bool enable) {
  _otaContext.rollbackEnabled = enable;
}

void MQTTOTA::enableVersionCheck(bool enable) {
  _otaContext.versionCheckEnabled = enable;
}
