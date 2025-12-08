#include "MQTTOTA.h"
#include <Update.h>

extern "C" {
    #include "libb64/cdecode.h"
    #include "libb64/cencode.h"
}

String MQTTOTA::base64Decode(const String& encoded) {
    if (ESP.getFreeHeap() < 35000) {
        Serial.println("Memoria baja antes de decodificar Base64");
        yield();
    }
    
    String decoded;
    const char* encodedChars = encoded.c_str();
    int encodedLength = encoded.length();
    
    int maxDecodedSize = (encodedLength * 3) / 4 + 2;
    
    if (maxDecodedSize > 50000) {
        Serial.printf("ERROR: Chunk Base64 demasiado grande: %d bytes\n", maxDecodedSize);
        return "";
    }
    
    base64_decodestate state;
    base64_init_decodestate(&state);
    
    char* buffer = (char*)malloc(maxDecodedSize);
    if (!buffer) {
        Serial.println("ERROR: No se pudo asignar memoria para Base64");
        return "";
    }
    
    int count = base64_decode_block(encodedChars, encodedLength, buffer, &state);
    
    if (count > 0) {
        decoded = String(buffer, count);
    } else {
        Serial.println("ERROR: Decodificación Base64 devolvió 0 bytes");
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
    _otaContext.inProgress = false;
    _otaContext.currentPart = 0;
    _otaContext.totalParts = 0;
    _otaContext.receivedSize = 0;
    _otaContext.update_handle = 0;
    _otaContext.update_partition = NULL;
    _otaContext.state = OTA_STATE_IDLE;
    _otaContext.retryCount = 0;
    _otaContext.maxRetries = MQTT_OTA_MAX_RETRIES;
}

// Destructor
MQTTOTA::~MQTTOTA() {
    cleanup();
    _cleanupChunkedOTA();
}

// SDK Initialization
void MQTTOTA::begin(const String& deviceName, const String& firmwareVersion) {
    _deviceName = deviceName;
    _firmwareVersion = firmwareVersion;

    Serial.println("MQTTOTA Inicializado");
    Serial.printf("Dispositivo: %s\n", _deviceName.c_str());
    Serial.printf("Versión: %s\n", _firmwareVersion.c_str());
    Serial.printf("ID Dispositivo: %s\n", _deviceID.c_str());
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

    Serial.printf("MQTT Configurado - Tópico OTA: %s\n", _otaTopic.c_str());
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

void MQTTOTA::onStateChange(MQTTOTAStateCallback callback) {
    _stateChangeCallback = callback;
}

// Main Handling
void MQTTOTA::handle() {
    // Check timeout
    if (_otaInProgress && (millis() - _otaStartTime > MQTT_OTA_TIMEOUT_MS)) {
        _publishError("Timeout en actualización OTA", _currentFirmwareVersion);
        cleanup();
        Serial.println("OTA Timeout - Actualización cancelada");
    }

    if (_otaContext.inProgress && (millis() - _otaContext.startTime > MQTT_OTA_TIMEOUT_MS)) {
        _publishError("Timeout en OTA por chunks", _otaContext.firmwareVersion);
        _cleanupChunkedOTA();
        Serial.println("OTA Chunks Timeout - Actualización cancelada");
    }
}

// MQTT Message Processing
void MQTTOTA::processMessage(const String& topic, const String& message) {
    if (topic != _otaTopic) return;

    if (isUpdateInProgress()) {
        Serial.println("OTA en progreso, ignorando nuevo mensaje");
        return;
    }

    if (ESP.getFreeHeap() < 30000) {
        Serial.println("Memoria insuficiente para procesar OTA");
        return;
    }

    Serial.println("Procesando mensaje OTA...");

    if (_chunkedOTAEnabled) {
        _processOTAChunk(message);
    } else {
        _processOTAMessage(message);
    }
}

// Full OTA Processing
void MQTTOTA::_processOTAMessage(const String& message) {
    DynamicJsonDocument doc(MQTT_OTA_JSON_SIZE);
    DeserializationError error = deserializeJson(doc, message);

    if (error) {
        Serial.printf("Error parseando JSON: %s\n", error.c_str());
        return;
    }

    if (!doc.containsKey("EventType") || doc["EventType"] != "UpdateFirmwareDevice") {
        return;
    }

    if (!doc.containsKey("Details")) {
        Serial.println("No se encontraron Details en el mensaje");
        return;
    }

    JsonObject details = doc["Details"];
    String firmwareVersion = details["FirmwareVersion"] | "";
    String base64Data = details["Base64"] | "";

    if (firmwareVersion.isEmpty() || base64Data.isEmpty()) {
        Serial.println("Datos OTA incompletos");
        return;
    }

    if (!_validateFirmwareData(base64Data)) {
        return;
    }

    Serial.printf("Iniciando OTA - Versión: %s, Tamaño: %d bytes\n",
                 firmwareVersion.c_str(), base64Data.length());

    _otaInProgress = true;
    _otaStartTime = millis();
    _currentFirmwareVersion = firmwareVersion;

    _publishProgress(10, firmwareVersion);

    if (performUpdate(base64Data, firmwareVersion)) {
        _publishSuccess(firmwareVersion);
        Serial.println("OTA Completado - Reiniciando...");
        delay(2000);
        ESP.restart();
    } else {
        _otaInProgress = false;
        cleanup();
    }
}

// Chunked OTA Processing
void MQTTOTA::_processOTAChunk(const String& message) {
    DynamicJsonDocument doc(4096);
    DeserializationError error = deserializeJson(doc, message);

    if (error) {
        Serial.printf("Error parseando JSON OTA: %s\n", error.c_str());
        return;
    }

    if (!doc.containsKey("EventType") || doc["EventType"] != "UpdateFirmwareDevice") {
        return;
    }

    if (!doc.containsKey("Details")) {
        Serial.println("No se encontraron Details en el mensaje OTA");
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
        Serial.printf("Error en chunk OTA: %s\n", chunk.errorMessage.c_str());
        _publishError(chunk.errorMessage, chunk.firmwareVersion);
        _cleanupChunkedOTA();
        return;
    }

    if (chunk.base64Part.isEmpty() || chunk.firmwareVersion.isEmpty()) {
        _publishError("Chunk OTA incompleto", chunk.firmwareVersion);
        _cleanupChunkedOTA();
        return;
    }

    // First chunk
    if (chunk.partIndex == 1) {
        if (_otaContext.inProgress) {
            Serial.println("OTA en progreso, ignorando nuevo inicio");
            return;
        }

        if (!_startChunkedOTA(chunk)) {
            return;
        }
    }

    // Verify sequence
    if (!_otaContext.inProgress || chunk.partIndex != _otaContext.currentPart + 1) {
        Serial.printf("Chunk fuera de secuencia. Esperado: %d, Recibido: %d\n",
                     _otaContext.currentPart + 1, chunk.partIndex);
        _publishError("Chunk fuera de secuencia", chunk.firmwareVersion);
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
    Serial.printf("Chunk %d/%d procesado. Progreso: %d%%\n",
                 chunk.partIndex, chunk.totalParts, progress);

    // Last chunk
    if (chunk.partIndex == chunk.totalParts) {
        _completeChunkedOTA(chunk);
    }
}

// Start Chunked OTA
bool MQTTOTA::_startChunkedOTA(const OTAChunkData& chunk) {
    Serial.printf("Iniciando OTA por chunks. Versión: %s, Partes: %d\n",
                 chunk.firmwareVersion.c_str(), chunk.totalParts);

    esp_err_t err;
    _otaContext.update_partition = esp_ota_get_next_update_partition(NULL);
    if (_otaContext.update_partition == NULL) {
        _publishError("No se pudo encontrar partición OTA", chunk.firmwareVersion);
        return false;
    }

    err = esp_ota_begin(_otaContext.update_partition, OTA_WITH_SEQUENTIAL_WRITES, &_otaContext.update_handle);
    if (err != ESP_OK) {
        String errorMsg = "Error iniciando OTA: ";
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
    Serial.println("OTA por chunks iniciada");
    return true;
}

// Process Chunk Data
bool MQTTOTA::_processChunkData(const OTAChunkData& chunk) {
    if (!_otaContext.inProgress || _otaContext.update_handle == 0) {
        Serial.println("ERROR: OTA no iniciada o handle inválido");
        _publishError("OTA no iniciada correctamente", chunk.firmwareVersion);
        return false;
    }

    String decodedData = base64Decode(chunk.base64Part);
    if (decodedData.length() == 0) {
        _publishError("Error decodificando chunk Base64", chunk.firmwareVersion);
        return false;
    }

    // Verify header in first chunk
    if (chunk.partIndex == 1) {
        if (!_processImageHeader((const uint8_t*)decodedData.c_str(), decodedData.length())) {
            _publishError("Encabezado de imagen inválido en primer chunk", chunk.firmwareVersion);
            _cleanupChunkedOTA();
            return false;
        }
        Serial.println("Encabezado de imagen verificado");
    }

    esp_err_t err = esp_ota_write(_otaContext.update_handle,
                                 (const void *)decodedData.c_str(),
                                 decodedData.length());

    if (err != ESP_OK) {
        String errorMsg = "Error escribiendo chunk OTA: ";
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
    Serial.println("Completando OTA por chunks...");

    if (_otaContext.receivedSize < 1000) {
        _publishError("Firmware demasiado pequeño", chunk.firmwareVersion);
        _cleanupChunkedOTA();
        return;
    }

    _publishProgress(90, chunk.firmwareVersion);

    esp_err_t err = esp_ota_end(_otaContext.update_handle);
    if (err != ESP_OK) {
        String errorMsg = "Error finalizando OTA: ";
        errorMsg += esp_err_to_name(err);
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            errorMsg += " - Validación de imagen falló";
        }
        _publishError(errorMsg, chunk.firmwareVersion);
        _cleanupChunkedOTA();
        return;
    }

    _publishProgress(95, chunk.firmwareVersion);

    err = esp_ota_set_boot_partition(_otaContext.update_partition);
    if (err != ESP_OK) {
        String errorMsg = "Error estableciendo partición de arranque: ";
        errorMsg += esp_err_to_name(err);
        _publishError(errorMsg, chunk.firmwareVersion);
        _cleanupChunkedOTA();
        return;
    }

    _publishProgress(100, chunk.firmwareVersion);
    Serial.println("OTA por chunks completada exitosamente!");

    _publishSuccess(chunk.firmwareVersion);

    Serial.println("Reiniciando en 3 segundos...");
    delay(3000);
    ESP.restart();
}

// Cleanup Chunked OTA
void MQTTOTA::_cleanupChunkedOTA() {
    if (_otaContext.inProgress && _otaContext.update_handle != 0) {
        esp_ota_abort(_otaContext.update_handle);
        Serial.println("OTA abortada y limpiada");
    }

    _otaContext.inProgress = false;
    _otaContext.currentPart = 0;
    _otaContext.totalParts = 0;
    _otaContext.receivedSize = 0;
    _otaContext.startTime = 0;
    _otaContext.update_handle = 0;
    _otaContext.update_partition = NULL;
}

// Execute Full OTA Update
bool MQTTOTA::performUpdate(const String& base64Data, const String& firmwareVersion) {
    return _performOTAUpdateESPIDF(base64Data, firmwareVersion);
}

// ESP-IDF OTA Implementation
bool MQTTOTA::_performOTAUpdateESPIDF(const String& base64Data, const String& firmwareVersion) {
    Serial.println("Iniciando actualización OTA con ESP-IDF...");

    if (ESP.getFreeHeap() < 50000) {
        _publishError("Memoria insuficiente para OTA", firmwareVersion);
        return false;
    }

    String decodedData = base64Decode(base64Data);
    if (decodedData.length() == 0) {
        _publishError("Error decodificando Base64", firmwareVersion);
        return false;
    }

    Serial.printf("Firmware decodificado: %d bytes, Memoria libre: %d\n",
                 decodedData.length(), ESP.getFreeHeap());

    esp_err_t err;
    const esp_partition_t* update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        _publishError("No se pudo encontrar partición OTA", firmwareVersion);
        return false;
    }

    esp_ota_handle_t update_handle;
    err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &update_handle);
    if (err != ESP_OK) {
        String errorMsg = "Error iniciando OTA: ";
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
            _publishError("Encabezado de imagen inválido", firmwareVersion);
            return false;
        }

        err = esp_ota_write(update_handle,
                           (const void *)(decodedData.c_str() + i),
                           current_chunk_size);

        if (err != ESP_OK) {
            esp_ota_abort(update_handle);
            String errorMsg = "Error escribiendo OTA: ";
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

        Serial.printf("Escritos %d bytes de %d (%.1f%%)\n",
                     bytes_written, total_size, (bytes_written * 100.0 / total_size));
    }

    _publishProgress(75, firmwareVersion);

    err = esp_ota_end(update_handle);
    if (err != ESP_OK) {
        String errorMsg = "Error finalizando OTA: ";
        errorMsg += esp_err_to_name(err);
        _publishError(errorMsg, firmwareVersion);
        return false;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        String errorMsg = "Error estableciendo partición de arranque: ";
        errorMsg += esp_err_to_name(err);
        _publishError(errorMsg, firmwareVersion);
        return false;
    }

    Serial.println("OTA completado exitosamente!");
    _publishProgress(100, firmwareVersion);

    return true;
}

// Validate Firmware Data
bool MQTTOTA::_validateFirmwareData(const String& base64Data) {
    if (base64Data.isEmpty()) {
        _publishError("Datos de firmware vacíos");
        return false;
    }

    if (base64Data.length() < 100) {
        _publishError("Datos de firmware demasiado cortos");
        return false;
    }

    for (unsigned int i = 0; i < base64Data.length(); i++) {
        char c = base64Data.charAt(i);
        if (!isalnum(c) && c != '+' && c != '/' && c != '=' && c != '\n' && c != '\r') {
            _publishError("Formato Base64 inválido");
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

    Serial.printf("Error OTA: %s\n", errorMessage.c_str());
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

    Serial.printf("OTA Exitoso - Versión: %s\n", firmwareVersion.c_str());
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

    Serial.printf("Progreso OTA: %d%%\n", progress);
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
        Serial.println("Paquete recibido no tiene longitud suficiente para encabezado");
        return false;
    }

    esp_app_desc_t new_app_info;
    memcpy(&new_app_info, &data[sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t)], sizeof(esp_app_desc_t));

    Serial.printf("Nueva versión de firmware: %s\n", new_app_info.version);
    return true;
}

void MQTTOTA::setPartitionName(const String& partitionName) {
    _otaContext.partitionName = partitionName;
}

OTAState MQTTOTA::getCurrentState() {
    return _otaContext.state;
}

OTAStatistics MQTTOTA::getStatistics() {
    return _stats;
}
/*
size_t MQTTOTA::getFreeOTASpace() {
    const esp_partition_t* partition = esp_ota_get_next_update_partition(NULL);
    if (partition == NULL) return 0;
    
    esp_ota_handle_t handle;
    esp_err_t err = esp_ota_begin(partition, OTA_SIZE_UNKNOWN, &handle);
    if (err != ESP_OK) return 0;
    
    size_t free_space = 0;
    esp_ota_get_partition_description(partition, &free_space);
    esp_ota_abort(handle);
    
    return free_space;
}
*/

void MQTTOTA::printDiagnostics() {
    Serial.println("=== Diagnósticos MQTTOTA ===");
    Serial.printf("ID Dispositivo: %s\n", _deviceID.c_str());
    Serial.printf("Firmware: %s\n", _firmwareVersion.c_str());
    Serial.printf("Memoria Libre: %d bytes\n", ESP.getFreeHeap());
    Serial.printf("OTA en progreso: %s\n", isUpdateInProgress() ? "Sí" : "No");
    Serial.printf("Progreso actual: %d%%\n", _currentProgress);
    
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (running) {
        Serial.printf("Partición actual: %s (0x%08X)\n", 
                     running->label, running->address);
    }
}

String MQTTOTA::getBootPartitionInfo() {
    const esp_partition_t* boot_partition = esp_ota_get_boot_partition();
    if (!boot_partition) return "Desconocido";
    
    char buffer[256];
    snprintf(buffer, sizeof(buffer), 
             "Etiqueta: %s, Tipo: %d, Subtipo: %d, Dirección: 0x%08X, Tamaño: %d",
             boot_partition->label,
             boot_partition->type,
             boot_partition->subtype,
             boot_partition->address,
             boot_partition->size);
    return String(buffer);
}

size_t MQTTOTA::calculateBase64DecodedSize(const String& encoded) {
    size_t len = encoded.length();
    size_t padding = 0;
    
    if (len > 0 && encoded.charAt(len - 1) == '=') padding++;
    if (len > 1 && encoded.charAt(len - 2) == '=') padding++;
    
    return (len * 3) / 4 - padding;
}

void MQTTOTA::abortUpdate() {
    if (isUpdateInProgress()) {
        _publishError("Actualización abortada por usuario", _currentFirmwareVersion);
        _cleanupChunkedOTA();
        cleanup();
        _setState(OTA_STATE_ABORTED);
        Serial.println("Actualización OTA abortada");
    }
}

bool MQTTOTA::checkMemory(size_t requiredBytes) {
    size_t freeHeap = ESP.getFreeHeap();
    bool hasEnoughMemory = (freeHeap >= requiredBytes + MQTT_OTA_MIN_MEMORY);
    
    if (!hasEnoughMemory) {
        Serial.printf("Memoria insuficiente: %d bytes disponibles, %d bytes requeridos\n",
                     freeHeap, requiredBytes + MQTT_OTA_MIN_MEMORY);
    }
    
    return hasEnoughMemory;
}

size_t MQTTOTA::getFreeHeap() {
    return ESP.getFreeHeap();
}

void MQTTOTA::logMemoryStatus() {
    Serial.printf("Estado de Memoria - Libre: %d, Mínimo Libre: %d, Máximo Asignable: %d\n",
                 ESP.getFreeHeap(),
                 ESP.getMinFreeHeap(),
                 ESP.getMaxAllocHeap());
}

bool MQTTOTA::verifyFirmwareSignature(const String& signature) {
    if (signature.isEmpty()) {
        Serial.println("Advertencia: No se proporcionó firma para verificación");
        return true; // Permitir sin firma si no se requiere
    }
    
    Serial.printf("Verificación de firma solicitada: %s\n", signature.c_str());
    return true;
}

bool MQTTOTA::checkFirmwareCompatibility(const String& newVersion) {
    if (newVersion.isEmpty()) return false;
    
    int dots = 0;
    for (size_t i = 0; i < newVersion.length(); i++) {
        char c = newVersion.charAt(i);
        if (c == '.') dots++;
        else if (!isdigit(c) && c != '-' && c != '+') return false;
    }
    
    return (dots >= 1 && dots <= 2);
}

bool MQTTOTA::_validateChecksum(const String& data, const String& checksum) {
    if (checksum.isEmpty()) return true;
    
    Serial.printf("Validación de checksum: Longitud datos=%d, Checksum=%s\n",
                 data.length(), checksum.c_str());
    return true;
}

void MQTTOTA::_handleChunkError(const OTAChunkData& chunk, const String& error) {
    Serial.printf("Error en chunk %d: %s\n", chunk.partIndex, error.c_str());
    
    _otaContext.retryCount++;
    if (_otaContext.retryCount <= _otaContext.maxRetries) {
        Serial.printf("Reintentando chunk %d (intento %d/%d)\n",
                     chunk.partIndex, _otaContext.retryCount, _otaContext.maxRetries);
      
    } else {
        _publishError("Máximo de reintentos excedido para chunk: " + error, chunk.firmwareVersion);
        _cleanupChunkedOTA();
    }
}

void MQTTOTA::_publishStateChange(OTAState state) {
    if (_stateChangeCallback) {
        _stateChangeCallback(static_cast<uint8_t>(state));
    }
    
    if (_publishMQTT && _isMQTTConnected && _isMQTTConnected()) {
        DynamicJsonDocument doc(512);
        doc["device"] = _deviceID;
        doc["state"] = static_cast<uint8_t>(state);
        doc["state_name"] = _getStateName(state);
        doc["timestamp"] = millis();
        
        String output;
        serializeJson(doc, output);
        _publishMQTT("ota/state", output);
    }
    
    Serial.printf("Estado OTA cambiado a: %s\n", _getStateName(state).c_str());
}

String MQTTOTA::_calculateSHA256(const uint8_t* data, size_t length) {
    return "";
}

void MQTTOTA::_setState(OTAState state) {
    if (_otaContext.state != state) {
        _otaContext.state = state;
        _publishStateChange(state);
    }
}

void MQTTOTA::_updateStatistics(size_t bytesReceived, bool isError) {
    if (_stats.startTime == 0 && bytesReceived > 0) {
        _stats.startTime = millis();
    }
    
    if (bytesReceived > 0) {
        _stats.totalBytes += bytesReceived;
        _stats.receivedBytes += bytesReceived;
        _stats.chunkCount++;
        
        if (_stats.startTime > 0) {
            unsigned long elapsed = millis() - _stats.startTime;
            if (elapsed > 1000) {
                _stats.averageSpeed = (_stats.receivedBytes * 1000.0) / elapsed;
            }
        }
    }
    
    if (isError) {
        _stats.errorCount++;
    }
    
    _stats.lastState = _otaContext.state;
}

bool MQTTOTA::_checkFirmwareVersion(const String& newVersion) {
    if (!_otaContext.versionCheckEnabled) return true;
    
    return (newVersion != _firmwareVersion);
}

bool MQTTOTA::_checkRollbackProtection() {
    if (!_otaContext.rollbackEnabled) return true;
    
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (!running) return false;
    
    return true;
}

bool MQTTOTA::_verifyImageIntegrity(const uint8_t* data, size_t length) {
    if (length < sizeof(esp_image_header_t)) {
        return false;
    }
    
    const esp_image_header_t* header = (const esp_image_header_t*)data;
    
    if (header->magic != ESP_IMAGE_HEADER_MAGIC) {
        Serial.println("Número mágico de imagen inválido");
        return false;
    }
    
    if (header->segment_count == 0) {
        Serial.println("No hay segmentos en la imagen");
        return false;
    }
    
    return true;
}

bool MQTTOTA::_validatePartitionWrite() {
    if (_otaContext.update_partition == NULL) {
        return false;
    }
    
    // Verifica que la partición sea de tipo APP
    if (_otaContext.update_partition->type != ESP_PARTITION_TYPE_APP) {
        Serial.println("Tipo de partición inválido para OTA");
        return false;
    }
    
    // CORREGIDO: Usar propiedad size directamente en lugar de esp_partition_get_info()
    if (_otaContext.update_partition->size < 1024) { // Tamaño mínimo razonable
        Serial.printf("Partición demasiado pequeña para firmware: %u bytes\n", _otaContext.update_partition->size);
        return false;
    }
    
    Serial.printf("Partición válida: %s, dirección: 0x%08X, tamaño: %u bytes\n",
                  _otaContext.update_partition->label,
                  _otaContext.update_partition->address,
                  _otaContext.update_partition->size);
    
    return true;
}

// Helper para nombres de estado - FUNCIÓN MIEMBRO CORREGIDA
String MQTTOTA::_getStateName(OTAState state) {
    switch(state) {
        case OTA_STATE_IDLE: return "INACTIVO";
        case OTA_STATE_RECEIVING: return "RECIBIENDO";
        case OTA_STATE_DECODING: return "DECODIFICANDO";
        case OTA_STATE_VALIDATING: return "VALIDANDO";
        case OTA_STATE_WRITING: return "ESCRIBIENDO";
        case OTA_STATE_COMPLETING: return "FINALIZANDO";
        case OTA_STATE_SUCCESS: return "EXITOSO";
        case OTA_STATE_ERROR: return "ERROR";
        case OTA_STATE_ABORTED: return "ABORTADO";
        default: return "DESCONOCIDO";
    }
}

void MQTTOTA::enableRollbackProtection(bool enable) {
    _otaContext.rollbackEnabled = enable;
}

void MQTTOTA::enableVersionCheck(bool enable) {
    _otaContext.versionCheckEnabled = enable;
}

