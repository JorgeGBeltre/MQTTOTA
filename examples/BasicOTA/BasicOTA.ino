#include <WiFi.h>
#include <PubSubClient.h>
#include "MQTTOTA.h"

// WiFi Configuration
const char* ssid = "YourSSID";
const char* password = "YourPassword";

// MQTT Configuration
const char* mqtt_server = "broker.hivemq.com"; // Your broker
const int mqtt_port = 1883;
const char* mqtt_user = "";
const char* mqtt_password = "";
const char* mqtt_client_id = "ESP32_Device_001";

// Topics
const char* ota_topic = "devices/ota";
const char* status_topic = "devices/status";
const char* telemetry_topic = "devices/telemetry";

// Instances
WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);
MQTTOTA ota;

// Connection management variables
unsigned long lastReconnectAttempt = 0;
const unsigned long RECONNECT_INTERVAL = 5000;

// Function to connect to WiFi
void setupWiFi() {
    Serial.print("Connecting to WiFi");
    WiFi.begin(ssid, password);
    
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    
    Serial.println("\nWiFi connected!");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());
}

// Function to connect/reconnect to MQTT
bool reconnectMQTT() {
    if (mqttClient.connect(mqtt_client_id, mqtt_user, mqtt_password)) {
        Serial.println("Connected to MQTT broker");
        
        // Subscribe to topics
        mqttClient.subscribe(ota_topic);
        Serial.printf("Subscribed to: %s\n", ota_topic);
        
        // Publish online status
        String status_msg = "{\"device\":\"" + String(mqtt_client_id) + 
                           "\",\"status\":\"online\",\"version\":\"" + 
                           ota.getCurrentVersion() + "\"}";
        mqttClient.publish(status_topic, status_msg.c_str());
        
        return true;
    }
    return false;
}

// MQTT Callback
void mqttCallback(char* topic, byte* payload, unsigned int length) {
    String message;
    for (unsigned int i = 0; i < length; i++) {
        message += (char)payload[i];
    }
    
    Serial.printf("Message received [%s]: %s\n", topic, message.c_str());
    
    // Pass message to OTA system
    ota.processMessage(String(topic), message);
}

void setup() {
    Serial.begin(115200);
    delay(1000);
    
    Serial.println("=== MQTT OTA System ===");
    
    // 1. Initialize WiFi
    setupWiFi();
    
    // 2. Configure MQTT
    mqttClient.setServer(mqtt_server, mqtt_port);
    mqttClient.setCallback(mqttCallback);
    mqttClient.setBufferSize(4096);  // Large buffer for OTA messages
    
    // 3. Initialize MQTTOTA
    ota.begin("MyDevice", "1.0.0");
    
    // 4. Configure OTA callbacks
    ota.onProgress([](int progress, const String& version) {
        Serial.printf("OTA Progress: %d%%, Version: %s\n", progress, version.c_str());
        
        // Publish progress via MQTT
        String progress_msg = "{\"device\":\"" + String(mqtt_client_id) + 
                             "\",\"version\":\"" + version + 
                             "\",\"progress\":" + String(progress) + 
                             ",\"type\":\"progress\"}";
        mqttClient.publish(status_topic, progress_msg.c_str());
    });
    
    ota.onError([](const String& error, const String& version) {
        Serial.printf("OTA Error: %s (Version: %s)\n", error.c_str(), version.c_str());
        
        // Publish error via MQTT
        String error_msg = "{\"device\":\"" + String(mqtt_client_id) + 
                          "\",\"version\":\"" + version + 
                          "\",\"error\":\"" + error + 
                          "\",\"type\":\"error\"}";
        mqttClient.publish(status_topic, error_msg.c_str());
    });
    
    ota.onSuccess([](const String& version) {
        Serial.printf("OTA successful! New version: %s\n", version.c_str());
        
        // Publish success via MQTT
        String success_msg = "{\"device\":\"" + String(mqtt_client_id) + 
                           "\",\"version\":\"" + version + 
                           "\",\"status\":\"update_completed\"," +
                           "\"type\":\"success\"}";
        mqttClient.publish(status_topic, success_msg.c_str());
    });
    
    // 5. Configure MQTT for OTA
    ota.setMQTTConfig(
        [](const char* topic, const String& message) {
            // Function to publish MQTT
            if (mqttClient.connected()) {
                mqttClient.publish(topic, message.c_str());
                Serial.printf("Published to %s: %s\n", topic, message.c_str());
            }
        },
        []() {
            // Function to verify MQTT connection
            return mqttClient.connected();
        },
        ota_topic
    );
    
    // 6. Configure OTA options (optional)
    ota.enableChunkedOTA(true);      // Enable chunked OTA
    ota.setChunkSize(2048);          // Chunk size: 2KB
    ota.setAutoReset(true);          // Auto-reset after OTA
    ota.setMaxRetries(3);            // Maximum 3 retries
    
    Serial.println("Setup completed!");
    Serial.printf("Device ID: %s\n", ota.getDeviceID().c_str());
    Serial.printf("Current Version: %s\n", ota.getCurrentVersion().c_str());
    
    // Diagnostic information
    Serial.printf("Free memory: %d bytes\n", ESP.getFreeHeap());
}

void loop() {
    // Maintain MQTT connection
    if (!mqttClient.connected()) {
        unsigned long now = millis();
        if (now - lastReconnectAttempt > RECONNECT_INTERVAL) {
            lastReconnectAttempt = now;
            if (reconnectMQTT()) {
                lastReconnectAttempt = 0;
            } else {
                Serial.println("Failed to reconnect to MQTT");
            }
        }
    } else {
        mqttClient.loop();
    }
    
    // Handle OTA (checks timeouts, etc.)
    ota.handle();
    
    // Publish periodic telemetry
    static unsigned long lastTelemetry = 0;
    if (millis() - lastTelemetry > 30000) {  // Every 30 seconds
        lastTelemetry = millis();
        
        if (mqttClient.connected()) {
            String telemetry = "{";
            telemetry += "\"device\":\"" + String(mqtt_client_id) + "\",";
            telemetry += "\"uptime\":" + String(millis() / 1000) + ",";
            telemetry += "\"free_heap\":" + String(ESP.getFreeHeap()) + ",";
            telemetry += "\"version\":\"" + ota.getCurrentVersion() + "\",";
            telemetry += "\"ota_in_progress\":" + String(ota.isUpdateInProgress() ? "true" : "false") + ",";
            telemetry += "\"wifi_rssi\":" + String(WiFi.RSSI()) + ",";
            telemetry += "\"type\":\"telemetry\"";
            telemetry += "}";
            
            mqttClient.publish(telemetry_topic, telemetry.c_str());
            Serial.println("Telemetry published");
        }
    }
    
    delay(100);
}

// Helper function to check memory
void checkMemory() {
    if (ESP.getFreeHeap() < 20000) {
        Serial.println("WARNING: Low memory!");
    }
}
