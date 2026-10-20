#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <ArduinoJson.h>

// --- Librerías para Flasheo por WiFi (OTA) ---
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <WiFiUdp.h>

// --- Credenciales (WiFi, OTA y API key) en secrets.h, fuera de git ---
#include "secrets.h"

// --- Credenciales WiFi ---
const char* ssid = WIFI_SSID;
const char* password = WIFI_PASSWORD;

// --- Configuración del Backend ---
const char* serverName = "https://termo-sync-backend.vercel.app/api/sensor"; 
const String branchId = "6aa967d7b062e946de93aa1b"; // ID de la sucursal "Al Fuego Dalvian"

// --- Configuración de los Sensores DS18B20 ---
const int oneWireBus = 5; // Pin donde conectaste el cable de datos (D4 / GPIO4)
OneWire oneWire(oneWireBus);
DallasTemperature sensors(&oneWire);

// --- Cronómetro para OTA (Reemplaza al delay) ---
unsigned long lastTelemetryTime = 0;
const unsigned long telemetryInterval = 900000; // 15 minutos en milisegundos

void setup() {
  Serial.begin(115200);
  sensors.begin(); // Inicializamos el bus de los DS18B20
  
  WiFi.begin(ssid, password);
  Serial.print("Conectando a WiFi");
  while(WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nConectado a la red WiFi");
  Serial.print("Dirección MAC de la placa: ");
  Serial.println(WiFi.macAddress());

  // --- Inicializar OTA ---
  ArduinoOTA.setHostname("ESP32-Dalvian"); 
  
  ArduinoOTA.setPassword(OTA_PASSWORD);

  ArduinoOTA.begin();
  Serial.println("OTA Iniciado. Listo para recibir flasheo por WiFi.");
}

void loop() {
  // 1. Escucha activa de OTA (debe ejecutarse siempre sin delays que bloqueen la placa)
  ArduinoOTA.handle();

  // 2. Evaluamos si ya pasaron los 15 minutos (o si es el primer arranque)
  if (millis() - lastTelemetryTime >= telemetryInterval || lastTelemetryTime == 0) {
    lastTelemetryTime = millis(); // Reiniciamos el cronómetro

    if(WiFi.status() == WL_CONNECTED) {
      sensors.requestTemperatures();
      
      // 3. Documento JSON expandido a 2048 bytes para soportar más de 11 sensores
      StaticJsonDocument<2048> doc; 
      
      doc["deviceId"] = WiFi.macAddress();
      doc["branchId"] = branchId;
      JsonArray readings = doc.createNestedArray("readings");

      DeviceAddress address;
      oneWire.reset_search(); // Reinicia el puntero de búsqueda en el cable físico

      // 4. Búsqueda dinámica y filtro anti-ruido
      while (oneWire.search(address)) {
        
        // Verificación de CRC: Si un bit se deforma por ruido del motor, ignora la lectura
        if (OneWire::crc8(address, 7) != address[7]) {
          Serial.println("Ruido detectado en el bus, ignorando ID corrupto...");
          continue;
        }

        float temp = sensors.getTempC(address);

        // Filtro de errores de hardware: Ignoramos el -127 (desconectado) y el 85 (error de encendido)
        if (temp > -50.0 && temp < 80.0) {
          
          // Convertir la dirección a formato String hexadecimal
          String mac = "";
          for (uint8_t j = 0; j < 8; j++) {
            if (address[j] < 16) mac += "0";
            mac += String(address[j], HEX);
          }
          mac.toUpperCase();

          // Agregamos el objeto al array de lecturas
          JsonObject sensorData = readings.createNestedObject();
          sensorData["address"] = mac;
          sensorData["temp"] = temp;
        }
      }

      // Si todos los sensores fallaron por ruido extremo o corte de cable, no mandamos un array vacío
      if (readings.size() == 0) {
        Serial.println("Error: No se lograron lecturas válidas en este ciclo. Abortando envío.");
        return; 
      }

      // 5. Convertir y enviar el payload
      String jsonPayload;
      serializeJson(doc, jsonPayload);
      Serial.println("Payload a enviar:");
      Serial.println(jsonPayload);

      WiFiClientSecure client;
      client.setInsecure(); // Fundamental para HTTPS

      HTTPClient http;
      http.setTimeout(15000);
      http.begin(client, serverName); 
      http.addHeader("Content-Type", "application/json");
      http.addHeader("x-api-key", SENSOR_API_KEY);

      int httpResponseCode = http.POST(jsonPayload);
      Serial.print("Código de respuesta HTTP: ");
      Serial.println(httpResponseCode);
      
      http.end();
    } else {
      Serial.println("WiFi desconectado. Intentando en el próximo ciclo.");
    }
  }
}