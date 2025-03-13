#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <BH1750.h>
#include <Adafruit_Sensor.h>
#include <DHT.h>
#include <DHT_U.h>
#include <Wire.h>
#include "credentials.h"
#include <EEPROM.h>

#define EEPROM_SIZE 96
#define MAX_WIFI_ATTEMPTS 10

const int digitalPort = 5;
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
#define DHTPIN 4
#define DHTTYPE DHT11
DHT_Unified dht(DHTPIN, DHTTYPE);

// HTTP server
ESP8266WebServer server(80);
String mqttTopic;

String scanWiFiNetworks()
{
    int n = WiFi.scanNetworks(); // Perform WiFi scan
    Serial.println("Scanning for WiFi networks...");

    String json = "[";
    for (int i = 0; i < n; i++)
    {
        if (i > 0)
            json += ","; // Add a comma for JSON formatting
        json += "{\"ssid\":\"" + WiFi.SSID(i) + "\",";
        json += "\"encryption\":" + String(WiFi.encryptionType(i)) + "}";
    }
    json += "]";

    Serial.println("Scan complete:");
    Serial.println(json); // Debug output

    return json;
}

void saveWiFiCredentials(const String &ssid, const String &password)
{
    EEPROM.begin(EEPROM_SIZE);
    for (int i = 0; i < ssid.length(); ++i)
    {
        EEPROM.write(i, ssid[i]);
    }
    EEPROM.write(ssid.length(), '\0');
    for (int i = 0; i < password.length(); ++i)
    {
        EEPROM.write(32 + i, password[i]);
    }
    EEPROM.write(32 + password.length(), '\0');
    EEPROM.commit();
}

void loadWiFiCredentials()
{
    EEPROM.begin(EEPROM_SIZE);
    client_ssid = "";
    client_password = "";
    for (int i = 0; i < 32; ++i)
    {
        char c = EEPROM.read(i);
        if (c == '\0')
            break;
        client_ssid += c;
    }
    for (int i = 32; i < 96; ++i)
    {
        char c = EEPROM.read(i);
        if (c == '\0')
            break;
        client_password += c;
    }
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
    WiFi.begin(client_ssid.c_str(), client_password.c_str());
    Serial.println("Connecting to WiFi...");

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < MAX_WIFI_ATTEMPTS)
    {
        delay(500);
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

    server.on("/connect", HTTP_POST, []()
              {
        if (server.hasArg("ssid") && server.hasArg("password")) {
            client_ssid = server.arg("ssid");
            client_password = server.arg("password");
            saveWiFiCredentials(client_ssid, client_password);
            WiFi.softAPdisconnect(true);
            if (!connectToWiFi()) {
                startAP();
            }
        } else {
            server.send(400, "text/plain", "Missing SSID or password.");
        } });

    server.begin();
    Serial.println("HTTP server started in AP mode.");
}

void setup()
{
    Serial.begin(115200);
    EEPROM.begin(EEPROM_SIZE);
    pinMode(digitalPort, INPUT);
    pinMode(analogPort, INPUT);
    dht.begin();
    Wire.begin(D2, D1);
    lightMeter.begin();
    loadWiFiCredentials();
    mqttTopic = "/sensors/" + WiFi.macAddress();
    if (!connectToWiFi())
    {
        startAP();
    }
}

void loop()
{
    if (WiFi.status() == WL_CONNECTED)
    {
        client.loop();
        JsonDocument data;
        int portValue = digitalRead(digitalPort);
        float soil = analogRead(analogPort);
        float lux = lightMeter.readLightLevel();
        sensors_event_t event;
        dht.temperature().getEvent(&event);
        float temperature = isnan(event.temperature) ? 0 : event.temperature;
        dht.humidity().getEvent(&event);
        float humidity = isnan(event.relative_humidity) ? 0 : event.relative_humidity;
        data["temp"] = temperature;
        data["humidity"] = humidity;
        data["soil"] = soil;
        data["light"] = lux;
        char payload[100];
        serializeJson(data, payload, sizeof(payload));
        client.publish(mqttTopic.c_str(), payload);
        delay(5000);
    }
    else
    {
        server.handleClient();
    }
}