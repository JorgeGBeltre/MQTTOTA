#include "MQTTOTA.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>

const char *WIFI_SSID = "your-ssid";
const char *WIFI_PASS = "your-password";

const char *MQTT_HOST = "your-broker.example.com";
const uint16_t MQTT_PORT = 8883;
const char *MQTT_CLIENT = "esp32-ota-secure";
const char *MQTT_USER = "ota-user";
const char *MQTT_PASS = "ota-password";
const char *DEVICE_NAME = "my-secure-device";
const char *FW_VERSION = "1.2.0";

const char *HMAC_KEY = "my-super-secret-key";

/*
const char *BROKER_CA_CERT = \
"-----BEGIN CERTIFICATE-----\n" \
"Your_CERTIFICATE_HERE\n" \
"-----END CERTIFICATE-----\n";
*/

WiFiClientSecure wifiClient;
PubSubClient mqtt(wifiClient);
MQTTOTA ota;

void connectWiFi() {
  Serial.printf("WiFi: connecting to %s\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print('.');
  }
  Serial.printf("\nWiFi: %s\n", WiFi.localIP().toString().c_str());
}

void connectMQTT() {
  // Configure TLS
  // wifiClient.setCACert(BROKER_CA_CERT);
  // For development with self-signed certs, use:
  wifiClient.setInsecure(); // WARNING: disables certificate verification!

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback([](char* topic, byte* payload, unsigned int length) {
    String message;
    for (unsigned int i = 0; i < length; i++) {
      message += (char)payload[i];
    }
    ota.processMessage(String(topic), message);
  });

  while (!mqtt.connected()) {
    Serial.println("MQTT: connecting...");
    if (mqtt.connect(MQTT_CLIENT, MQTT_USER, MQTT_PASS)) {
      Serial.println("MQTT: connected (TLS)");
      mqtt.subscribe("ota/my-secure-device");
    } else {
      Serial.print("failed, rc=");
      Serial.print(mqtt.state());
      Serial.println(" try again in 5 seconds");
      delay(5000);
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== SecureOTA ===");

  ota.begin(DEVICE_NAME, FW_VERSION);
  ota.setSecurityKey(HMAC_KEY);
  ota.setSecurityMode(SECURITY_HMAC_SHA256);

  // NOTE: To use ECDSA signature, you would do:
  // ota.setSecurityMode(SECURITY_ECDSA_SHA256);
  // ota.setPublicKey(MY_ECDSA_PUBLIC_KEY);

  ota.setMQTTConfig(
    [](const char* topic, const String& msg) { mqtt.publish(topic, msg.c_str()); },
    []() -> bool { return mqtt.connected(); },
    "ota/my-secure-device"
  );

  ota.onProgress([](int pct, const String &ver) {
    Serial.printf("OTA [%d%%] version=%s heap=%u\n", pct, ver.c_str(), ESP.getFreeHeap());
  });

  ota.onError([](const String &err, const String &ver) {
    Serial.printf("OTA ERROR: %s (version=%s)\n", err.c_str(), ver.c_str());
  });

  ota.onSuccess([](const String &ver) {
    Serial.printf("OTA SUCCESS: %s\n", ver.c_str());
  });

  ota.onStateChange([](uint8_t s) { Serial.printf("OTA state → %d\n", s); });

  connectWiFi();
  connectMQTT();
}

void loop() {
  if (!mqtt.connected()) {
    connectMQTT();
  }
  mqtt.loop();
  ota.handle();

  static unsigned long lastDiag = 0;
  if (ota.isUpdateInProgress() && millis() - lastDiag > 30000) {
    ota.printDiagnostics();
    lastDiag = millis();
  }

  delay(10);
}
