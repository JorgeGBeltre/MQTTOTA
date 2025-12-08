#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include <ArduinoJson.h>
#include "mqtt_client.h"
#include "MQTTOTA.h" 

// For development, you can uncomment this line to skip SSL verification
// #define SKIP_SSL_VERIFICATION

// Firmware version
const char* FIRMWARE_VERSION = "v1.0.0";

// WiFi Configuration
const unsigned long WIFI_CONNECT_TIMEOUT = 3000;

// NTP Configuration
const char* ntpServer1 = "pool.ntp.org";
const char* ntpServer2 = "time.nist.gov";
const long gmtOffset_sec = -4 * 3600;
const int daylightOffset_sec = 0;

// MQTT Configuration
const char* MQTT_URI = "broker.hivemq.com"; // Your broker
const char* API_KEY = ""; // Your API_KEY
const unsigned long MQTT_RECONNECT_INTERVAL = 10000;
const int MQTT_SOCKET_TIMEOUT = 300;
const size_t MQTT_BUFFER_SIZE = 40960;
const unsigned long MQTT_KEEP_ALIVE = 300;

// If you need to use SSL certificate, uncomment and define your certificate here:
/*
const char* root_ca_pem = \
"-----BEGIN CERTIFICATE-----\n" \
"Your_CERTIFICATE_HERE\n" \
"-----END CERTIFICATE-----\n";
*/

// MQTT topics configuration
String MQTT_TOPIC_EVENTS = "events";

// Global variables
esp_mqtt_client_handle_t mqtt_client = NULL;
bool mqttConnected = false;
unsigned long lastReconnectAttempt = 0;
unsigned long previousMillis = 0;
const uint8_t wifiLed = 2;

// Device information
struct DeviceInfo {
  String chipId;
  String chipType;
  String macAddress;
  String clientID;
  String firmwareVersion;
  String groupName = ""; // If you have different device types (switches, sensors, etc.)
} device;

// MQTTOTA instance
MQTTOTA mqttOTA;

// WiFi statuses
enum WiFiStatus {
  WIFI_DISCONNECTED,
  WIFI_CONNECTING,
  WIFI_CONNECTED
};
WiFiStatus currentWiFiStatus = WIFI_DISCONNECTED;

// Function prototypes
void setupWiFi();
void updateWiFiStatus();
void reconnectWiFi();
void updateWiFiLED();
void handleButton();
String getMQTTClientID();
String getISOTimestamp();
DeviceInfo getDeviceInfo();
void publishEvent(const char* event, const char* description);
void publishMQTTMessage(const char* topic, const String& message);
void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data);
bool connectMQTT();
bool configureNTPTime();
void verifyAndSyncTime();
String extractMessageFromPayload(const String& message);
String getDeviceEventTopic();

void setupWiFi() {
  Serial.println("Starting WiFi configuration...");
  
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(true);

  currentWiFiStatus = WIFI_CONNECTING;

  if (WiFi.SSID().length() > 0) {
    Serial.println("Saved WiFi credentials found. Connecting...");
    Serial.println("SSID: " + WiFi.SSID());
    
    WiFi.begin();
    
    unsigned long startAttemptTime = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < WIFI_CONNECT_TIMEOUT) {
      delay(500);
      Serial.print(".");
    }
    
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\nWiFi connected successfully!");
      Serial.println("IP address: " + WiFi.localIP().toString());
      currentWiFiStatus = WIFI_CONNECTED;
      return;
    } else {
      Serial.println("\nError: Could not connect with saved credentials");
    }
  } else {
    Serial.println("No saved WiFi credentials found.");
  }

  Serial.println("Use physical button (hold 3 seconds) to configure WiFi");
  currentWiFiStatus = WIFI_DISCONNECTED;
}

void updateWiFiStatus() {
  if (WiFi.status() != WL_CONNECTED) {
    if (currentWiFiStatus == WIFI_CONNECTED) {
      Serial.println("WiFi disconnected");
      currentWiFiStatus = WIFI_DISCONNECTED;
      if (mqttConnected) {
        publishEvent("WIFI_DISCONNECTED", "WiFi connection lost");
      }
    }
    reconnectWiFi();
  }
}

void reconnectWiFi() {
  if (WiFi.status() != WL_CONNECTED && currentWiFiStatus != WIFI_CONNECTING) {
    Serial.println("Reconnecting WiFi...");
    currentWiFiStatus = WIFI_CONNECTING;
    WiFi.begin();
  } 
  else if (WiFi.status() == WL_CONNECTED && currentWiFiStatus != WIFI_CONNECTED) {
    Serial.println("WiFi reconnected");
    currentWiFiStatus = WIFI_CONNECTED;
    if (mqttConnected) {
      publishEvent("WIFI_RECONNECTED", "WiFi connection restored successfully");
    }
  }
}

void updateWiFiLED() {
  static bool ledState = LOW;
  unsigned long currentMillis = millis();
  const int FAST_BLINK_INTERVAL = 100;
  const int SLOW_BLINK_INTERVAL = 400;

  if (currentMillis - previousMillis >= 
      (currentWiFiStatus == WIFI_DISCONNECTED ? FAST_BLINK_INTERVAL : 
       currentWiFiStatus == WIFI_CONNECTING ? SLOW_BLINK_INTERVAL : 0)) {
    
    previousMillis = currentMillis;
    
    if (currentWiFiStatus == WIFI_CONNECTED) {
      digitalWrite(wifiLed, HIGH);
    } else {
      ledState = !ledState;
      digitalWrite(wifiLed, ledState);
    }
  }
}

DeviceInfo getDeviceInfo() {
  DeviceInfo info;

  uint64_t chipID = ESP.getEfuseMac();
  char chipIdStr[17];
  snprintf(chipIdStr, sizeof(chipIdStr), "%04X%08X",
           (uint16_t)(chipID >> 32), (uint32_t)chipID);

  info.chipId = String(chipIdStr);
  info.chipType = String(ESP.getChipModel());
  info.macAddress = WiFi.macAddress();
  info.firmwareVersion = FIRMWARE_VERSION;
  info.clientID = info.chipId;
  info.clientID.toUpperCase();

  Serial.printf("Device information:\n");
  Serial.printf("  Chip ID: %s\n", info.chipId.c_str());
  Serial.printf("  MAC Address: %s\n", info.macAddress.c_str());
  Serial.printf("  Chip Type: %s\n", info.chipType.c_str());
  Serial.printf("  Firmware: %s\n", info.firmwareVersion.c_str());

  return info;
}

String getMQTTClientID() {
  return device.clientID;
}

String getDeviceEventTopic() {
  return "event/" + device.chipId;
}

String getISOTimestamp() {
    struct tm timeinfo;
    time_t now;
    time(&now);
    
    if (now < 1000000000) {
        unsigned long ms = millis();
        char fallback[30];
        snprintf(fallback, sizeof(fallback), "2025-01-01T00:00:%02lu.%03luZ", 
                (ms / 1000) % 60, ms % 1000);
        return String(fallback);
    }
    
    if (!getLocalTime(&timeinfo)) {
        return "2025-01-01T00:00:00.000Z";
    }
    
    uint32_t ms = millis() % 1000;
    char timestamp[25];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%S", &timeinfo);
    
    char fullTimestamp[30];
    snprintf(fullTimestamp, sizeof(fullTimestamp), "%s.%03ldZ", timestamp, ms);
    
    return String(fullTimestamp);
}

String extractMessageFromPayload(const String& message) {
    // This function extracts the actual message from the payload
    // Assumes the message may come inside a JSON object with "Payload" key
    
    DynamicJsonDocument doc(32768);
    DeserializationError error = deserializeJson(doc, message);
    
    if (error) {
        Serial.printf("Error parsing JSON to extract payload: %s\n", error.c_str());
        return message; // Return original message if cannot parse
    }
    
    // Check if there is a "Payload" key
    if (doc.containsKey("Payload")) {
        JsonObject payloadObj = doc["Payload"];
        DynamicJsonDocument actualDoc(16384);
        actualDoc.set(payloadObj);
        String actualMessage;
        serializeJson(actualDoc, actualMessage);
        return actualMessage;
    }
    
    // If no "Payload", return original message
    return message;
}

void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    
    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED: {
            Serial.println("MQTT CONNECTED!");
            mqttConnected = true;
            
            // Subscribe to device event topic
            String deviceEventTopic = getDeviceEventTopic();
            esp_mqtt_client_subscribe(mqtt_client, deviceEventTopic.c_str(), 1);
            Serial.printf("Subscribed to: %s\n", deviceEventTopic.c_str());
            
            // Publish connection event
            publishEvent("MQTT_CONNECTED", "MQTT connection established");
            break;
        }
            
        case MQTT_EVENT_DISCONNECTED:
            Serial.println("MQTT disconnected");
            mqttConnected = false;
            break;
            
        case MQTT_EVENT_DATA: {
            if (ESP.getFreeHeap() < 20000) {
                Serial.println("Insufficient memory to process MQTT message");
                break;
            }
            
            String topic(event->topic, event->topic_len);
            String message(event->data, event->data_len);
            
            Serial.printf("Topic received: %s\n", topic.c_str());
            Serial.printf("Payload length: %d bytes\n", event->data_len);
            
            // Process device events
            String deviceEventTopic = getDeviceEventTopic();
            if (topic == deviceEventTopic) {
                Serial.println("Device event received");
                
                // Extract actual message from payload if needed
                String actualMessage = extractMessageFromPayload(message);
                
                // First try to process as OTA
                mqttOTA.processMessage(topic, actualMessage);
                
                // Also check for specific commands
                DynamicJsonDocument doc(32768);
                DeserializationError error = deserializeJson(doc, actualMessage);
                
                if (!error && doc.containsKey("EventType")) {
                    String eventType = doc["EventType"].as<String>();
                    Serial.printf("EventType detected: %s\n", eventType.c_str());
                    
                    if (eventType == "REBOOT") {
                        publishEvent("SYSTEM_REBOOT", "Restart requested by API");
                        delay(1000);
                        ESP.restart();
                    }
                    else if (eventType == "GET_STATUS") {
                        publishEvent("STATUS_REQUEST", "Status request received");
                        // Here you could publish more system information
                    }
                }
            }
            break;
        }
            
        default:
            break;
    }
}

bool connectMQTT() {
    if (mqttConnected) return true;
    
    if (mqtt_client != NULL) {
        esp_mqtt_client_destroy(mqtt_client);
        mqtt_client = NULL;
    }

    String clientId = getMQTTClientID();
    Serial.printf("Connecting MQTT as %s...\n", clientId.c_str());

    esp_mqtt_client_config_t mqtt_cfg = {};
    mqtt_cfg.broker.address.uri = MQTT_URI;
    mqtt_cfg.credentials.client_id = clientId.c_str();
    
    // If you have API_KEY, use it as username
    if (strlen(API_KEY) > 0) {
        mqtt_cfg.credentials.username = API_KEY;
    }
    
    // Option 1: Use SSL certificate (uncomment if needed)
    // mqtt_cfg.broker.verification.certificate = root_ca_pem;
    
    // Option 2: No SSL verification (for development)
    #ifdef SKIP_SSL_VERIFICATION
        mqtt_cfg.broker.verification.skip_cert_common_name_check = true;
    #endif
    
    mqtt_cfg.session.keepalive = MQTT_KEEP_ALIVE;
    mqtt_cfg.network.timeout_ms = MQTT_SOCKET_TIMEOUT * 1000;
    mqtt_cfg.buffer.size = MQTT_BUFFER_SIZE;
    mqtt_cfg.buffer.out_size = 4096;

    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqtt_client, MQTT_EVENT_ANY, mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);

    return true;
}

void publishMQTTMessage(const char* topic, const String& message) {
    if (!mqttConnected || mqttOTA.isUpdateInProgress()) return;
    
    int msg_id = esp_mqtt_client_publish(mqtt_client, topic, message.c_str(), message.length(), 1, 0);
    if (msg_id < 0) {
        Serial.printf("Error publishing to %s (msg_id: %d)\n", topic, msg_id);
    } else {
        Serial.printf("Published to %s (%d bytes)\n", topic, message.length());
    }
}

void publishEvent(const char* event, const char* description) {
    StaticJsonDocument<768> doc;
    
    JsonObject deviceObj = doc.createNestedObject("Device");
    deviceObj["status"] = mqttConnected ? "connected" : "disconnected";
    deviceObj["ChipId"] = device.chipId;
    deviceObj["MacAddress"] = device.macAddress;
    deviceObj["IPAddress"] = WiFi.localIP().toString();
    deviceObj["ChipType"] = device.chipType;
    deviceObj["FirmwareVersion"] = device.firmwareVersion;
    deviceObj["GroupName"] = device.groupName;
    
    doc["Timestamp"] = getISOTimestamp();
    
    JsonObject details = doc.createNestedObject("Details");
    details["chipId"] = device.chipId;
    
    JsonObject info = details.createNestedObject("info");
    info["event"] = event;
    info["timestamp"] = millis();
   
    if (description && strlen(description) > 0) {
        info["description"] = description;
    }
    
    String eventData;
    serializeJson(doc, eventData);
    
    publishMQTTMessage(MQTT_TOPIC_EVENTS.c_str(), eventData);
    Serial.printf("Event published: %s - %s\n", event, description);
}

void handleButton() {
  constexpr uint8_t BUTTON_PIN = 0;
  constexpr uint16_t BUTTON_LONG_PRESS_TIME = 3000;
  
  static bool lastButtonState = HIGH;
  static unsigned long buttonPressStartTime = 0;
  static bool isButtonPressed = false;
  
  bool buttonState = digitalRead(BUTTON_PIN);
  unsigned long actualMillis = millis();

  if (buttonState == LOW && lastButtonState == HIGH) {
    buttonPressStartTime = actualMillis;
    isButtonPressed = true;
    Serial.println("Button pressed - hold 3 seconds for AP mode");
  } 
  
  else if (buttonState == HIGH && lastButtonState == LOW && isButtonPressed) {
    unsigned long pressDuration = actualMillis - buttonPressStartTime;
    isButtonPressed = false;

    if (pressDuration > BUTTON_LONG_PRESS_TIME) {
      Serial.println("Activating AP mode via physical button...");
      
      String description = "Access Point mode activated by physical button - ";
      description += (WiFi.SSID().length() > 0) ? "With saved credentials" : "No saved credentials";
      publishEvent("AP_MODE_ACTIVATED", description.c_str());
      
      WiFiManager wifiManager;
      uint64_t chipID = ESP.getEfuseMac(); 
      String apSSID = "BASICOTA-" + String((uint16_t)(chipID >> 32), DEC) + String((uint32_t)chipID, DEC);
      
      wifiManager.setConfigPortalTimeout(180);
      wifiManager.startConfigPortal(apSSID.c_str(), "12345678");
      
      Serial.println("Restarting after AP configuration...");
      ESP.restart();
    }
  }

  lastButtonState = buttonState;
}

bool configureNTPTime() {
    Serial.println("Configuring NTP time...");
    
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer1, ntpServer2);
    
    Serial.print("Getting time from NTP");
    int attempts = 0;
    struct tm timeinfo;
    
    while (attempts < 30) {
        delay(1000);
        Serial.print(".");
        
        if (getLocalTime(&timeinfo)) {
            time_t now;
            time(&now);
            if (now > 1000000000) {
                Serial.println("\n NTP synchronized successfully!");
                return true;
            }
        }
        attempts++;
    }
    
    Serial.println("\nError: Could not sync with NTP after 30 seconds");
    return false;
}

void verifyAndSyncTime() {
    static unsigned long lastVerification = 0;
    const unsigned long TIME_SYNC_INTERVAL = 3600000;
    
    if (millis() - lastVerification > TIME_SYNC_INTERVAL) {
        time_t now = time(nullptr);
        if (now < 1000000000) {
            Serial.println("Re-synchronizing NTP time...");
            configureNTPTime();
        }
        lastVerification = millis();
    }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n=== BasicOTA with MQTTOTA ===");
  Serial.println("Firmware Version: " + String(FIRMWARE_VERSION));

  // Configure WiFi LED
  pinMode(wifiLed, OUTPUT);
  digitalWrite(wifiLed, LOW);
  
  // Configure button for AP mode
  constexpr uint8_t BUTTON_PIN = 0;
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  
  // Configure WiFi
  setupWiFi();
  
  // Configure time if WiFi is connected
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("Initializing time synchronization...");
    if (configureNTPTime()) {
      Serial.println("NTP configured successfully");
      Serial.print("First timestamp: ");
      Serial.println(getISOTimestamp());
    } else {
      Serial.println("Error configuring NTP");
    }
  }
  
  // Get device information
  device = getDeviceInfo();
  
  // Initialize MQTTOTA - topic will be configured dynamically
  mqttOTA.begin("BasicOTA", FIRMWARE_VERSION);
  
  // Configure MQTT for MQTTOTA with dynamic topic
  mqttOTA.setMQTTConfig(
      [](const char* topic, const String& message) {
          publishMQTTMessage(topic, message);
      },
      []() -> bool {
          return mqttConnected;
      },
      getDeviceEventTopic()  // Use dynamic event topic
  );
  
  // Configure optional callbacks
  mqttOTA.onProgress([](int progress, const String& version) {
      Serial.printf("OTA Progress: %d%% - Version: %s\n", progress, version.c_str());
      publishEvent("OTA_PROGRESS", String("Progress: " + String(progress) + "%").c_str());
  });
  
  mqttOTA.onError([](const String& error, const String& version) {
      Serial.printf("OTA Error: %s - Version: %s\n", error.c_str(), version.c_str());
      publishEvent("OTA_ERROR", error.c_str());
  });
  
  mqttOTA.onSuccess([](const String& version) {
      Serial.printf("OTA completed successfully - Version: %s\n", version.c_str());
      publishEvent("OTA_SUCCESS", "Update completed successfully");
  });
  
  Serial.println("\nBasicOTA - Ready for MQTT OTA updates");
  Serial.println("Client ID: " + device.clientID);
  Serial.println("Event topic: " + getDeviceEventTopic());
  Serial.println("AP mode: (press button for 3 seconds)");
  
  // Publish startup event
  publishEvent("SYSTEM_START", "BasicOTA system started correctly");
}

void loop() {
    // Handle WiFi
    updateWiFiLED();
    updateWiFiStatus();
    
    // Handle button for AP mode
    handleButton();
    
    // Handle MQTTOTA
    mqttOTA.handle();

    // Connect MQTT if WiFi is connected
    if (WiFi.status() == WL_CONNECTED) {
        if (!mqttConnected) {
            unsigned long now = millis();
            if (now - lastReconnectAttempt > MQTT_RECONNECT_INTERVAL) {
                lastReconnectAttempt = now;
                connectMQTT();
            }
        }
    }
    
    // Verify and sync time periodically
    verifyAndSyncTime();
    
    // Publish heartbeat periodically (every 5 minutes)
    static unsigned long lastHeartbeat = 0;
    if (mqttConnected && millis() - lastHeartbeat > 300000) {  // 5 minutes
        publishEvent("HEARTBEAT", "System functioning correctly");
        lastHeartbeat = millis();
    }
    
    delay(10);
}
