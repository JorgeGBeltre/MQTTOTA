```cpp
#ifndef MQTT_OTA_SDK_H
#define MQTT_OTA_SDK_H

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Update.h>
#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include "esp_partition.h"



extern "C" {
    #include "libb64/cdecode.h"
    #include "libb64/cencode.h"
}



#ifndef MQTT_OTA_BUFFSIZE
#define MQTT_OTA_BUFFSIZE 1024
#endif

#ifndef MQTT_OTA_HASH_LEN
#define MQTT_OTA_HASH_LEN 32
#endif

#ifndef MQTT_OTA_TIMEOUT_MS
#define MQTT_OTA_TIMEOUT_MS 420000  // 7 minutes
#endif

#ifndef MQTT_OTA_JSON_SIZE
#define MQTT_OTA_JSON_SIZE 32768
#endif

#ifndef MQTT_OTA_MAX_CHUNK_SIZE
#define MQTT_OTA_MAX_CHUNK_SIZE 65536  // 64KB maximum per chunk
#endif

#ifndef MQTT_OTA_MIN_MEMORY
#define MQTT_OTA_MIN_MEMORY 40000     // Minimum required memory
#endif

#ifndef MQTT_OTA_MAX_RETRIES
#define MQTT_OTA_MAX_RETRIES 3        // Maximum retries
#endif


// ENUM AND DATA STRUCTURES


// Callbacks for OTA events
typedef std::function<void(int progress, const String& version)> MQTTOTACallback;
typedef std::function<void(const String& error, const String& version)> MQTTOTAErrorCallback;
typedef std::function<void(const String& version)> MQTTOTASuccessCallback;
typedef std::function<void(uint8_t state)> MQTTOTAStateCallback;

// OTA process states
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
    float averageSpeed = 0.0;  // bytes/second
};


// MAIN MQTTOTA CLASS


class MQTTOTA {
public:
    // Constructor and Destructor
    MQTTOTA();
    ~MQTTOTA();

    // CONFIGURATION 
    
    /**
     * @brief Initializes the MQTTOTA SDK
     * @param deviceName Device name
     * @param firmwareVersion Current firmware version
     */
    void begin(const String& deviceName, const String& firmwareVersion);
    
    /**
     * @brief Configures MQTT connection
     * @param publishFunc Function to publish MQTT messages
     * @param isConnectedFunc Function to verify MQTT connection
     * @param otaTopic MQTT topic for OTA (default "ota")
     */
    void setMQTTConfig(
        std::function<void(const char* topic, const String& message)> publishFunc,
        std::function<bool()> isConnectedFunc,
        const String& otaTopic = "ota"
    );
    
    /**
     * @brief Configures custom partition for OTA
     * @param partitionName Partition name (optional)
     */
    void setPartitionName(const String& partitionName = "");

    //  CALLBACKS 
    
    void onProgress(MQTTOTACallback callback);
    void onError(MQTTOTAErrorCallback callback);
    void onSuccess(MQTTOTASuccessCallback callback);
    void onStateChange(MQTTOTAStateCallback callback);

    //  MAIN METHODS 
    
    void handle();
    void processMessage(const String& topic, const String& message);
    bool performUpdate(const String& base64Data, const String& firmwareVersion);
    
    // OTA CONFIGURATION 
    
    void enableChunkedOTA(bool enable = true);
    void setChunkSize(size_t chunkSize);
    void setAutoReset(bool autoReset = true);
    void setMaxRetries(int maxRetries);
    void enableRollbackProtection(bool enable = true);
    void enableVersionCheck(bool enable = true);

    // STATUS AND QUERY 
    
    bool isUpdateInProgress();
    bool isValidating() const;
    bool isWriting() const;
    String getCurrentVersion();
    String getDeviceID();
    int getProgress();
    OTAState getCurrentState();
    OTAStatistics getStatistics();
    size_t getFreeOTASpace();
    
    //  UTILITIES AND DIAGNOSTICS 
    
    void printDiagnostics();
    String getBootPartitionInfo();
    static String base64Decode(const String& encoded);
    static String base64Encode(const String& input);
    static size_t calculateBase64DecodedSize(const String& encoded);
    void cleanup();
    void abortUpdate();
    
    //  MEMORY MANAGEMENT 
    
    static bool checkMemory(size_t requiredBytes);
    static size_t getFreeHeap();
    static void logMemoryStatus();
    
    // SECURITY METHODS 
    
    bool verifyFirmwareSignature(const String& signature);
    bool checkFirmwareCompatibility(const String& newVersion);

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
        const esp_partition_t* update_partition = NULL;
        OTAState state = OTA_STATE_IDLE;
        int retryCount = 0;
        int maxRetries = MQTT_OTA_MAX_RETRIES;
        String partitionName;
        bool rollbackEnabled = true;
        bool versionCheckEnabled = true;
    };

    struct OTAChunkData {
        String firmwareVersion;
        String base64Part;
        int partIndex;
        int totalParts;
        bool isError;
        String errorMessage;
        String checksum;
        size_t decodedSize;
    };

    // Member variables
    String _deviceName;
    String _firmwareVersion;
    String _deviceID;
    String _otaTopic;
    OTAContext _otaContext;
    
    // Callbacks
    MQTTOTACallback _progressCallback = nullptr;
    MQTTOTAErrorCallback _errorCallback = nullptr;
    MQTTOTASuccessCallback _successCallback = nullptr;
    MQTTOTAStateCallback _stateChangeCallback = nullptr;
    
    // MQTT Callbacks
    std::function<void(const char* topic, const String& message)> _publishMQTT = nullptr;
    std::function<bool()> _isMQTTConnected = nullptr;
    
    // Configuration
    bool _chunkedOTAEnabled = true;
    bool _autoReset = true;
    size_t _chunkSize = MQTT_OTA_BUFFSIZE;
    
    // Status and statistics
    bool _otaInProgress = false;
    unsigned long _otaStartTime = 0;
    String _currentFirmwareVersion;
    int _currentProgress = 0;
    OTAStatistics _stats;
    
    // Private methods
    void _initialize();
    void _processOTAMessage(const String& message);
    void _processOTAChunk(const String& message);
    bool _validateFirmwareData(const String& base64Data);
    bool _validateChecksum(const String& data, const String& checksum);
    bool _performOTAUpdateESPIDF(const String& base64Data, const String& firmwareVersion);
    
    // Chunks OTA
    bool _startChunkedOTA(const OTAChunkData& chunk);
    bool _processChunkData(const OTAChunkData& chunk);
    void _completeChunkedOTA(const OTAChunkData& chunk);
    void _cleanupChunkedOTA();
    void _handleChunkError(const OTAChunkData& chunk, const String& error);
    
    // Communication
    void _publishError(const String& errorMessage, const String& firmwareVersion = "");
    void _publishSuccess(const String& firmwareVersion);
    void _publishProgress(int progress, const String& firmwareVersion);
    void _publishStateChange(OTAState state);
    
    // Utilities
    static void _printSHA256(const uint8_t* image_hash, const char* label);
    static bool _processImageHeader(const uint8_t* data, size_t data_len);
    static String _calculateSHA256(const uint8_t* data, size_t length);
    String _generateDeviceID();
    
    // State management
    void _setState(OTAState state);
    void _updateStatistics(size_t bytesReceived = 0, bool isError = false);
    
    // Security and validation
    bool _checkFirmwareVersion(const String& newVersion);
    bool _checkRollbackProtection();
    bool _verifyImageIntegrity(const uint8_t* data, size_t length);
    bool _validatePartitionWrite();
};

// Inline method implementations
inline bool MQTTOTA::isUpdateInProgress() { 
    return _otaInProgress || _otaContext.inProgress; 
}

inline bool MQTTOTA::isValidating() const { 
    return _otaContext.state == OTA_STATE_VALIDATING; 
}

inline bool MQTTOTA::isWriting() const { 
    return _otaContext.state == OTA_STATE_WRITING; 
}

inline String MQTTOTA::getCurrentVersion() { 
    return _firmwareVersion; 
}

inline String MQTTOTA::getDeviceID() { 
    return _deviceID; 
}

inline int MQTTOTA::getProgress() { 
    return _currentProgress; 
}

inline void MQTTOTA::enableChunkedOTA(bool enable) { 
    _chunkedOTAEnabled = enable; 
}

inline void MQTTOTA::setChunkSize(size_t chunkSize) { 
    _chunkSize = (chunkSize > 0 && chunkSize <= MQTT_OTA_MAX_CHUNK_SIZE) ? chunkSize : MQTT_OTA_BUFFSIZE; 
}

inline void MQTTOTA::setAutoReset(bool autoReset) { 
    _autoReset = autoReset; 
}

inline void MQTTOTA::setMaxRetries(int maxRetries) { 
    _otaContext.maxRetries = (maxRetries > 0) ? maxRetries : MQTT_OTA_MAX_RETRIES; 
}

#endif // MQTT_OTA_SDK_H
```
