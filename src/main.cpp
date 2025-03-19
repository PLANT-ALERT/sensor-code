#include <ESP8266WebServer.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <BH1750.h>
#include <Adafruit_Sensor.h>
#include <DHT.h>
#include <Wire.h>
#include "credentials.h"
#include <EEPROM.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>

#define MAX_WIFI_ATTEMPTS 100
#define EEPROM_SIZE 96
#define EEPROM_SSID_ADDR 0
#define EEPROM_PASS_ADDR 32

const int analogPort = A0;

// light sensor
BH1750 lightMeter;

// Global variables
String client_ssid = "";
String client_password = "";

// WiFi and MQTT clients
WiFiClient espClient;
PubSubClient client(espClient);

// DHT11
#define DHTPIN D5
#define DHTTYPE DHT11
float Temperature;
float Humidity;
DHT dht(DHTPIN, DHTTYPE);

// HTTP server
ESP8266WebServer server(80);
WiFiManager wifiManager;
String mqttTopic;

void saveWiFiCredentialsToEEPROM(const String &ssid, const String &password)
{
    for (unsigned int i = 0; i < 32; i++)
    {
        EEPROM.write(EEPROM_SSID_ADDR + i, i < (int)ssid.length() ? ssid[i] : 0);
        EEPROM.write(EEPROM_PASS_ADDR + i, i < (int)password.length() ? password[i] : 0);
    }
    EEPROM.commit();
}

void loadWiFiCredentialsFromEEPROM()
{
    char ssid[33], pass[33];
    for (int i = 0; i < 32; i++)
    {
        ssid[i] = EEPROM.read(EEPROM_SSID_ADDR + i);
        pass[i] = EEPROM.read(EEPROM_PASS_ADDR + i);
    }
    ssid[32] = 0;
    pass[32] = 0;
    client_ssid = String(ssid);
    client_password = String(pass);
}

String scanWiFiNetworks()
{
    int n = WiFi.scanNetworks();
    String json = "[";
    for (int i = 0; i < n; i++)
    {
        if (i > 0)
            json += ",";
        json += "{\"ssid\":\"" + WiFi.SSID(i) + "\",";
        json += "\"encryption\":" + String(WiFi.encryptionType(i)) + "}";
    }
    json += "]";
    return json;
}

bool isRegistered()
{
    if (WiFi.status() != WL_CONNECTED)
    {
        Serial.println("WiFi not connected.");
        return false;
    }

    WiFiClientSecure secureClient;
    HTTPClient http;
    String mac = WiFi.macAddress();
    mac.replace(":", "");
    String url = String(api_url) + "/sensors/am_i_registred/" + mac;
    Serial.print("Calling api:");
    Serial.println(url);

    secureClient.setInsecure();
    http.begin(secureClient, url);
    int httpCode = http.GET();

    Serial.print("HTTP Response code: ");
    Serial.println(httpCode);

    if (httpCode == 204)
    {
        Serial.println("Device is NOT registered. Clearing EEPROM and rebooting...");
        for (int i = 0; i < 512; i++)
        {
            EEPROM.write(i, 0);
        }
        EEPROM.commit();
        http.end();
        ESP.restart(); // Reboot the device
        return false;
    }

    http.end();

    if (httpCode == 200)
    {
        Serial.println("Device is registered.");
        return true;
    }

    Serial.println("Unexpected response. Not modifying EEPROM.");
    return false;
}

void startMQTT()
{
    client.setServer(default_mqtt_broker, default_mqtt_port);
    while (!client.connected())
    {
        String client_id = "esp8266-client-" + String(WiFi.macAddress());
        if (client.connect(client_id.c_str(), default_mqtt_username, default_mqtt_password))
        {
            Serial.println("Connected to MQTT broker!");
        }
        else
        {
            Serial.print("MQTT connection failed, state: ");
            Serial.println(client.state());
            delay(2000);
        }
    }
}

bool connectToWiFi()
{
    Serial.println(client_ssid.c_str());
    Serial.println(client_password.c_str());
    WiFi.begin(client_ssid.c_str(), client_password.c_str());
    Serial.println("Connecting to WiFi...");
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < MAX_WIFI_ATTEMPTS)
    {
        delay(1000);
        Serial.print(".");
        attempts++;
    }
    if (WiFi.status() == WL_CONNECTED)
    {
        Serial.println("\nConnected to WiFi!");
        Serial.print("IP Address: ");
        Serial.println(WiFi.localIP());
        startMQTT();
        return true;
    }
    Serial.println("\nFailed to connect to WiFi.");
    return false;
}

void startAP()
{
    String macAddress = WiFi.macAddress();
    macAddress.replace(":", "");
    macAddress = macAddress.substring(0, 5);
    WiFi.softAP(String(ap_ssid) + "-" + macAddress, ap_password);
    Serial.print("AP IP address: ");
    Serial.println(WiFi.softAPIP());

    server.on("/ssid", HTTP_GET, []()
              {
    String jsonResponse = scanWiFiNetworks();
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "application/json", jsonResponse); });

    server.on("/mac", HTTP_GET, []()
              {
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "text/plain", WiFi.macAddress()); });

    server.on("/connect", HTTP_GET, []()
              {
            WiFi.softAPdisconnect(true);
            if (!connectToWiFi()) {
                startAP();
      } });

    server.on("/saveCredentials", HTTP_POST, []()
              {
    if (server.hasArg("ssid") && server.hasArg("password")) {
      client_ssid = server.arg("ssid");
      client_password = server.arg("password");
      saveWiFiCredentialsToEEPROM(client_ssid, client_password);
      Serial.println(client_ssid);
      Serial.println(client_password);
      server.send(200, "text/plain", "ok");
    } else {
      server.send(400, "text/plain", "Missing SSID or password.");
    } });

    server.on("/health", HTTP_GET, []()
              { server.send(200, "ok"); });

    server.begin();
    Serial.println("HTTP server started in AP mode.");
}

void setup()
{
    Serial.begin(115200);
    EEPROM.begin(EEPROM_SIZE);
    pinMode(analogPort, INPUT);
    dht.begin();
    Wire.begin(D2, D1);
    lightMeter.begin();
    mqttTopic = "/sensors/" + WiFi.macAddress();
    loadWiFiCredentialsFromEEPROM();
    if (!connectToWiFi())
    {
        startAP();
    }
    else
    {
        isRegistered();
    }
}

void loop()
{
    if (WiFi.status() == WL_CONNECTED)
    {
        client.loop();
        JsonDocument data;

        float soil = analogRead(analogPort);
        float lux = lightMeter.readLightLevel();
        float temperature = dht.readTemperature();
        float humidity = dht.readHumidity();

        Serial.println("---- Sensor Readings ----");
        Serial.print("Soil Moisture (Analog Read): ");
        Serial.println(soil);

        Serial.print("Light Intensity (BH1750 Lux): ");
        Serial.println(lux);

        if (isnan(temperature) || isnan(humidity))
        {
            Serial.println("ERROR: Failed to read from DHT sensor!");
        }
        else
        {
            Serial.print("Temperature (DHT11): ");
            Serial.print(temperature);
            Serial.println("°C");

            Serial.print("Humidity (DHT11): ");
            Serial.print(humidity);
            Serial.println("%");
        }

        data["temp"] = temperature;
        data["humidity"] = humidity;
        data["soil"] = soil;
        data["light"] = lux;

        String payload;
        serializeJson(data, payload);
        client.publish(mqttTopic.c_str(), payload.c_str());
        delay(5000);
    }
    else
    {
        server.handleClient();
    }
}
