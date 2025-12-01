#include "MQTTOTA.h"
#include <Update.h>

extern "C" {
    #include "libb64/cdecode.h"
    #include "libb64/cencode.h"
}

String MQTTOTA::base64Decode(const String& encoded) {
    if (ESP.getFreeHeap() < 35000) {
        Serial.println("Low memory before decoding Base64");
        yield();
    }
    
    String decoded;
    const char* encodedChars = encoded.c_str();
    int encodedLength = encoded.length();
    
    int maxDecodedSize = (encodedLength * 3) / 4 + 2;
    
    if (maxDecodedSize > 50000) {
        Serial.printf("ERROR: Base64 chunk too large: %d bytes\n", maxDecodedSize);
        return "";
    }
    
    base64_decodestate state;
    base64_init_decodestate(&state);
    
    char* buffer = (char*)malloc(maxDecodedSize);
    if (!buffer) {
        Serial.println("ERROR: Could not allocate memory for Base64");
        return "";
    }
    
    int count = base64_decode_block(encodedChars, encodedLength, buffer, &state);
    
    if (count > 0) {
        decoded = String(buffer, count);
    } else {
        Serial.println("ERROR: Base64 decoding returned 0 bytes");
    }
    
    free(buffer);
    return decoded;
} 

String MQTTOTA::base64Encode(const String& input) {
    if (input.isEmpty()) return "";
    
    String encoded;
    const char* inputChars = input.c_str();
    int inputLength = input.length();
    
    int encodedLength = (inputLength + 2) / 3 * 4 + 1;
    char* buffer = (char*)malloc(encodedLength);
    if (!buffer) return "";
    
    base64_encodestate state;
    base64_init_encodestate(&state);
    
    int count = base64_encode_block(inputChars, inputLength, buffer, &state);
    count += base64_encode_blockend(buffer + count, &state);
    
    encoded = String(buffer, count);
    free(buffer);
    return encoded;
}



// Constructor
MQTTOTA::MQTTOTA() {
    _deviceID = _generateDeviceID();
}

// SDK Initialization
void MQTTOTA::begin(const String& deviceName, const String& firmwareVersion) {
    _deviceName = deviceName;
    _firmwareVersion = firmwareVersion;

    Serial.println("MQTTOTA Initialized");
    Serial.printf("Device: %s\n", _deviceName.c_str());
    Serial.printf("Version: %s\n", _firmwareVersion.c_str());
    Serial.printf("Device ID: %s\n", _deviceID.c_str());
}

// MQTT Configuration
void MQTTOTA::setMQTTConfig(
    std::function<void(const char* topic, const String& message)> publishFunc,
    std::function<bool()> isConnectedFunc,
    const String& otaTopic
) {
    _publishMQTT = publishFunc;
    _isMQTTConnected = isConnectedFunc;
    _otaTopic = otaTopic;

    Serial.printf("MQTT Configured - OTA Topic: %s\n", _otaTopic.c_str());
}

// Callback Configuration
void MQTTOTA::onProgress(MQTTOTACallback callback) {
    _progressCallback = callback;
}

void MQTTOTA::onError(MQTTOTAErrorCallback callback) {
    _errorCallback = callback;
}

void MQTTOTA::onSuccess(MQTTOTASuccessCallback callback) {
    _successCallback = callback;
}


// MAIN HANDLING


// Main Handling
void MQTTOTA::handle() {
    // Check timeout
    if (_otaInProgress && (millis() - _otaStartTime > MQTT_OTA_TIMEOUT_MS)) {
        _publishError("OTA update timeout", _currentFirmwareVersion);
        cleanup();
        Serial.println("OTA Timeout - Update cancelled");
    }

    if (_otaContext.inProgress && (millis() - _otaContext.startTime > MQTT_OTA_TIMEOUT_MS)) {
        _publishError("Chunked OTA timeout", _otaContext.firmwareVersion);
        _cleanupChunkedOTA();
        Serial.println("OTA Chunks Timeout - Update cancelled");
    }
}

// MQTT Message Processing
void MQTTOTA::processMessage(const String& topic, const String& message) {
    if (topic != _otaTopic) return;

    if (isUpdateInProgress()) {
        Serial.println("OTA in progress, ignoring new message");
        return;
    }

    if (ESP.getFreeHeap() < 30000) {
        Serial.println("Insufficient memory to process OTA");
        return;
    }

    Serial.println("Processing OTA message...");

    if (_chunkedOTAEnabled) {
        _processOTAChunk(message);
    } else {
        _processOTAMessage(message);
    }
}


// FULL OTA PROCESSING


// Full OTA Processing
void MQTTOTA::_processOTAMessage(const String& message) {
    DynamicJsonDocument doc(MQTT_OTA_JSON_SIZE);
    DeserializationError error = deserializeJson(doc, message);

    if (error) {
        Serial.printf("Error parsing JSON: %s\n", error.c_str());
        return;
    }

    if (!doc.containsKey("EventType") || doc["EventType"] != "UpdateFirmwareDevice") {
        return;
    }

    if (!doc.containsKey("Details")) {
        Serial.println("No Details found in message");
        return;
    }

    JsonObject details = doc["Details"];
    String firmwareVersion = details["FirmwareVersion"] | "";
    String base64Data = details["Base64"] | "";

    if (firmwareVersion.isEmpty() || base64Data.isEmpty()) {
        Serial.println("Incomplete OTA data");
        return;
    }

    /*
    // Optional: Check if same version
    if (firmwareVersion == _firmwareVersion) {
        _publishError("Already have this firmware version installed", firmwareVersion);
        return;
    }
    */

    if (!_validateFirmwareData(base64Data)) {
        return;
    }

    Serial.printf("Starting OTA - Version: %s, Size: %d bytes\n",
                 firmwareVersion.c_str(), base64Data.length());

    _otaInProgress = true;
    _otaStartTime = millis();
    _currentFirmwareVersion = firmwareVersion;

    _publishProgress(10, firmwareVersion);

    if (performUpdate(base64Data, firmwareVersion)) {
        _publishSuccess(firmwareVersion);
        Serial.println("OTA Completed - Rebooting...");
        delay(2000);
        ESP.restart();
    } else {
        _otaInProgress = false;
        cleanup();
    }
}


// CHUNKED OTA PROCESSING


// Chunked OTA Processing
void MQTTOTA::_processOTAChunk(const String& message) {
    DynamicJsonDocument doc(4096);
    DeserializationError error = deserializeJson(doc, message);

    if (error) {
        Serial.printf("Error parsing OTA JSON: %s\n", error.c_str());
        return;
    }

    if (!doc.containsKey("EventType") || doc["EventType"] != "UpdateFirmwareDevice") {
        return;
    }

    if (!doc.containsKey("Details")) {
        Serial.println("No Details found in OTA message");
        return;
    }

    JsonObject details = doc["Details"];
    OTAChunkData chunk;

    chunk.firmwareVersion = details["FirmwareVersion"].as<String>();
    chunk.base64Part = details["Base64Part"].as<String>();
    chunk.partIndex = details["PartIndex"].as<int>();
    chunk.totalParts = details["TotalParts"].as<int>();
    chunk.isError = details["IsError"] | false;
    chunk.errorMessage = details["ErrorMessage"] | "";

    if (chunk.isError) {
        Serial.printf("OTA chunk error: %s\n", chunk.errorMessage.c_str());
        _publishError(chunk.errorMessage, chunk.firmwareVersion);
        _cleanupChunkedOTA();
        return;
    }

    if (chunk.base64Part.isEmpty() || chunk.firmwareVersion.isEmpty()) {
        _publishError("Incomplete OTA chunk", chunk.firmwareVersion);
        _cleanupChunkedOTA();
        return;
    }

    // First chunk
    if (chunk.partIndex == 1) {
        if (_otaContext.inProgress) {
            Serial.println("OTA in progress, ignoring new start");
            return;
        }
        /*
        if (chunk.firmwareVersion == _firmwareVersion) {
            _publishError("Already have this firmware version installed", chunk.firmwareVersion);
            return;
        }
        */

        if (!_startChunkedOTA(chunk)) {
            return;
        }
    }

    // Verify sequence
    if (!_otaContext.inProgress || chunk.partIndex != _otaContext.currentPart + 1) {
        Serial.printf("Chunk out of sequence. Expected: %d, Received: %d\n",
                     _otaContext.currentPart + 1, chunk.partIndex);
        _publishError("Chunk out of sequence", chunk.firmwareVersion);
        _cleanupChunkedOTA();
        return;
    }

    // Process chunk
    if (!_processChunkData(chunk)) {
        _cleanupChunkedOTA();
        return;
    }

    _otaContext.currentPart = chunk.partIndex;
    int progress = (chunk.partIndex * 100) / chunk.totalParts;
    _currentProgress = progress;

    _publishProgress(progress, chunk.firmwareVersion);
    Serial.printf("Chunk %d/%d processed. Progress: %d%%\n",
                 chunk.partIndex, chunk.totalParts, progress);

    // Last chunk
    if (chunk.partIndex == chunk.totalParts) {
        _completeChunkedOTA(chunk);
    }
}

// Start Chunked OTA
bool MQTTOTA::_startChunkedOTA(const OTAChunkData& chunk) {
    Serial.printf("Starting chunked OTA. Version: %s, Parts: %d\n",
                 chunk.firmwareVersion.c_str(), chunk.totalParts);

    esp_err_t err;
    _otaContext.update_partition = esp_ota_get_next_update_partition(NULL);
    if (_otaContext.update_partition == NULL) {
        _publishError("Could not find OTA partition", chunk.firmwareVersion);
        return false;
    }

    err = esp_ota_begin(_otaContext.update_partition, OTA_WITH_SEQUENTIAL_WRITES, &_otaContext.update_handle);
    if (err != ESP_OK) {
        String errorMsg = "Error starting OTA: ";
        errorMsg += esp_err_to_name(err);
        _publishError(errorMsg, chunk.firmwareVersion);
        return false;
    }

    _otaContext.inProgress = true;
    _otaContext.firmwareVersion = chunk.firmwareVersion;
    _otaContext.currentPart = 0;
    _otaContext.totalParts = chunk.totalParts;
    _otaContext.startTime = millis();
    _otaContext.receivedSize = 0;

    _publishProgress(0, chunk.firmwareVersion);
    Serial.println("Chunked OTA started");
    return true;
}

// Process Chunk Data
bool MQTTOTA::_processChunkData(const OTAChunkData& chunk) {
    if (!_otaContext.inProgress || _otaContext.update_handle == 0) {
        Serial.println("ERROR: OTA not started or invalid handle");
        _publishError("OTA not started correctly", chunk.firmwareVersion);
        return false;
    }

    String decodedData = base64Decode(chunk.base64Part);
    if (decodedData.length() == 0) {
        _publishError("Error decoding chunk Base64", chunk.firmwareVersion);
        return false;
    }

    // Verify header in first chunk
    if (chunk.partIndex == 1) {
        if (!_processImageHeader((const uint8_t*)decodedData.c_str(), decodedData.length())) {
            _publishError("Invalid image header in first chunk", chunk.firmwareVersion);
            _cleanupChunkedOTA();
            return false;
        }
        Serial.println("Image header verified");
    }

    esp_err_t err = esp_ota_write(_otaContext.update_handle,
                                 (const void *)decodedData.c_str(),
                                 decodedData.length());

    if (err != ESP_OK) {
        String errorMsg = "Error writing OTA chunk: ";
        errorMsg += esp_err_to_name(err);
        _publishError(errorMsg, chunk.firmwareVersion);
        return false;
    }

    _otaContext.receivedSize += decodedData.length();

    Serial.printf("Chunk %d: %d bytes. Total: %d bytes\n",
                 chunk.partIndex, decodedData.length(), _otaContext.receivedSize);

    return true;
}

// Complete Chunked OTA
void MQTTOTA::_completeChunkedOTA(const OTAChunkData& chunk) {
    Serial.println("Completing chunked OTA...");

    if (_otaContext.receivedSize < 1000) {
        _publishError("Firmware too small", chunk.firmwareVersion);
        _cleanupChunkedOTA();
        return;
    }

    _publishProgress(90, chunk.firmwareVersion);

    esp_err_t err = esp_ota_end(_otaContext.update_handle);
    if (err != ESP_OK) {
        String errorMsg = "Error finalizing OTA: ";
        errorMsg += esp_err_to_name(err);
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            errorMsg += " - Image validation failed";
        }
        _publishError(errorMsg, chunk.firmwareVersion);
        _cleanupChunkedOTA();
        return;
    }

    _publishProgress(95, chunk.firmwareVersion);

    err = esp_ota_set_boot_partition(_otaContext.update_partition);
    if (err != ESP_OK) {
        String errorMsg = "Error setting boot partition: ";
        errorMsg += esp_err_to_name(err);
        _publishError(errorMsg, chunk.firmwareVersion);
        _cleanupChunkedOTA();
        return;
    }

    _publishProgress(100, chunk.firmwareVersion);
    Serial.println("Chunked OTA completed successfully!");

    _publishSuccess(chunk.firmwareVersion);

    Serial.println("Rebooting in 3 seconds...");
    delay(3000);
    ESP.restart();
}

// Cleanup Chunked OTA
void MQTTOTA::_cleanupChunkedOTA() {
    if (_otaContext.inProgress && _otaContext.update_handle != 0) {
        esp_ota_abort(_otaContext.update_handle);
        Serial.println("OTA aborted and cleaned");
    }

    _otaContext.inProgress = false;
    _otaContext.currentPart = 0;
    _otaContext.totalParts = 0;
    _otaContext.receivedSize = 0;
    _otaContext.startTime = 0;
    _otaContext.update_handle = 0;
    _otaContext.update_partition = NULL;
}


// ESP-IDF OTA IMPLEMENTATION


// Execute Full OTA Update
bool MQTTOTA::performUpdate(const String& base64Data, const String& firmwareVersion) {
    return _performOTAUpdateESPIDF(base64Data, firmwareVersion);
}

// ESP-IDF OTA Implementation
bool MQTTOTA::_performOTAUpdateESPIDF(const String& base64Data, const String& firmwareVersion) {
    Serial.println("Starting OTA update with ESP-IDF...");

    if (ESP.getFreeHeap() < 50000) {
        _publishError("Insufficient memory for OTA", firmwareVersion);
        return false;
    }

    String decodedData = base64Decode(base64Data);
    if (decodedData.length() == 0) {
        _publishError("Error decoding Base64", firmwareVersion);
        return false;
    }

    Serial.printf("Decoded firmware: %d bytes, Free memory: %d\n",
                 decodedData.length(), ESP.getFreeHeap());

    esp_err_t err;
    const esp_partition_t* update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        _publishError("Could not find OTA partition", firmwareVersion);
        return false;
    }

    esp_ota_handle_t update_handle;
    err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &update_handle);
    if (err != ESP_OK) {
        String errorMsg = "Error starting OTA: ";
        errorMsg += esp_err_to_name(err);
        _publishError(errorMsg, firmwareVersion);
        return false;
    }

    _publishProgress(25, firmwareVersion);

    size_t total_size = decodedData.length();
    size_t bytes_written = 0;
    size_t chunk_size = _chunkSize;

    for (size_t i = 0; i < total_size; i += chunk_size) {
        size_t current_chunk_size = min(chunk_size, total_size - i);

        if (i == 0 && !_processImageHeader((const uint8_t*)decodedData.c_str(), current_chunk_size)) {
            esp_ota_abort(update_handle);
            _publishError("Invalid image header", firmwareVersion);
            return false;
        }

        err = esp_ota_write(update_handle,
                           (const void *)(decodedData.c_str() + i),
                           current_chunk_size);

        if (err != ESP_OK) {
            esp_ota_abort(update_handle);
            String errorMsg = "Error writing OTA: ";
            errorMsg += esp_err_to_name(err);
            _publishError(errorMsg, firmwareVersion);
            return false;
        }

        bytes_written += current_chunk_size;

        int progress = 25 + (bytes_written * 50 / total_size);
        if (progress > 75) progress = 75;

        if ((bytes_written * 100 / total_size) % 10 == 0) {
            _publishProgress(progress, firmwareVersion);
        }

        Serial.printf("Written %d bytes of %d (%.1f%%)\n",
                     bytes_written, total_size, (bytes_written * 100.0 / total_size));
    }

    _publishProgress(75, firmwareVersion);

    err = esp_ota_end(update_handle);
    if (err != ESP_OK) {
        String errorMsg = "Error finalizing OTA: ";
        errorMsg += esp_err_to_name(err);
        _publishError(errorMsg, firmwareVersion);
        return false;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        String errorMsg = "Error setting boot partition: ";
        errorMsg += esp_err_to_name(err);
        _publishError(errorMsg, firmwareVersion);
        return false;
    }

    Serial.println("OTA completed successfully!");
    _publishProgress(100, firmwareVersion);

    return true;
}


// VALIDATION AND UTILITIES


// Validate Firmware Data
bool MQTTOTA::_validateFirmwareData(const String& base64Data) {
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
        if (!isalnum(c) && c != '+' && c != '/' && c != '=' && c != '\n' && c != '\r') {
            _publishError("Invalid Base64 format");
            return false;
        }
    }

    return true;
}

// Publish Errors
void MQTTOTA::_publishError(const String& errorMessage, const String& firmwareVersion) {
    if (_errorCallback) {
        _errorCallback(errorMessage, firmwareVersion.isEmpty() ? _firmwareVersion : firmwareVersion);
    }

    if (_publishMQTT && _isMQTTConnected && _isMQTTConnected()) {
        DynamicJsonDocument doc(2048);
        doc["device"] = _deviceID;
        doc["version"] = firmwareVersion.isEmpty() ? _firmwareVersion : firmwareVersion;
        doc["error"] = errorMessage;
        doc["timestamp"] = millis();

        String output;
        serializeJson(doc, output);
        _publishMQTT("ota/error", output);
    }

    Serial.printf("OTA Error: %s\n", errorMessage.c_str());
}

// Publish Success
void MQTTOTA::_publishSuccess(const String& firmwareVersion) {
    if (_successCallback) {
        _successCallback(firmwareVersion);
    }

    if (_publishMQTT && _isMQTTConnected && _isMQTTConnected()) {
        DynamicJsonDocument doc(2048);
        doc["device"] = _deviceID;
        doc["version"] = firmwareVersion;
        doc["success"] = true;
        doc["timestamp"] = millis();

        String output;
        serializeJson(doc, output);
        _publishMQTT("ota/success", output);
    }

    Serial.printf("OTA Successful - Version: %s\n", firmwareVersion.c_str());
}

// Publish Progress
void MQTTOTA::_publishProgress(int progress, const String& firmwareVersion) {
    _currentProgress = progress;

    if (_progressCallback) {
        _progressCallback(progress, firmwareVersion);
    }

    if (_publishMQTT && _isMQTTConnected && _isMQTTConnected() && (progress % 10 == 0 || progress == 100)) {
        DynamicJsonDocument doc(1024);
        doc["device"] = _deviceID;
        doc["version"] = firmwareVersion;
        doc["progress"] = progress;
        doc["timestamp"] = millis();

        String output;
        serializeJson(doc, output);
        _publishMQTT("ota/progress", output);
    }

    Serial.printf("OTA Progress: %d%%\n", progress);
}

// Cleanup
void MQTTOTA::cleanup() {
    _otaInProgress = false;
    _currentProgress = 0;
    _otaStartTime = 0;
    _currentFirmwareVersion = "";
}

// Generate Device ID
String MQTTOTA::_generateDeviceID() {
    uint64_t chipID = ESP.getEfuseMac();
    char chipIdStr[17];
    snprintf(chipIdStr, sizeof(chipIdStr), "%04X%08X",
             (uint16_t)(chipID >> 32), (uint32_t)chipID);
    return String(chipIdStr);
}

// Internal Utilities
void MQTTOTA::_printSHA256(const uint8_t *image_hash, const char *label) {
    char hash_print[MQTT_OTA_HASH_LEN * 2 + 1];
    hash_print[MQTT_OTA_HASH_LEN * 2] = 0;
    for (int i = 0; i < MQTT_OTA_HASH_LEN; ++i) {
        sprintf(&hash_print[i * 2], "%02x", image_hash[i]);
    }
    Serial.printf("%s: %s\n", label, hash_print);
}

bool MQTTOTA::_processImageHeader(const uint8_t *data, size_t data_len) {
    if (data_len < sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t)) {
        Serial.println("Received packet not long enough for header");
        return false;
    }

    esp_app_desc_t new_app_info;
    memcpy(&new_app_info, &data[sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t)], sizeof(esp_app_desc_t));

    Serial.printf("New firmware version: %s\n", new_app_info.version);
    return true;
}
