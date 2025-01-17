#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <DHT_Async.h>
#include <credentials.h>

#define DHT_SENSOR_TYPE DHT_TYPE_11
#define DHT_SENSOR_PIN 4
const int digitalPort = 5;
const int analogPort = A0;

// Global variables
String client_ssid = "";
String client_password = "";

// WiFi and MQTT clients
WiFiClient espClient;
PubSubClient client(espClient);
DHT_Async dht_sensor(DHT_SENSOR_PIN, DHT_SENSOR_TYPE);

// HTTP server
ESP8266WebServer server(80);

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

void startAP()
{
  String macAddress = WiFi.macAddress();

  macAddress.replace(":", "");

  macAddress = macAddress.substring(0, 5);
  WiFi.softAP(ap_ssid + "-" + macAddress, ap_password);
  IPAddress IP = WiFi.softAPIP();
  Serial.print("AP IP address: ");
  Serial.println(IP);

  server.on("/health", HTTP_GET, []()
            {
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "text/plain", "OK"); });

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

        delay(1000);

        WiFi.softAPdisconnect(true);
        connectToWiFi();

        // Wait for the connection attempt
        unsigned long startAttemptTime = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 10000) {
            delay(500); // Wait and retry
        }

        if (WiFi.status() == WL_CONNECTED) {
            server.sendHeader("Access-Control-Allow-Origin", "*");
            server.send(200, "text/plain", WiFi.macAddress());
        } else {
            if (WiFi.status() == WL_CONNECT_FAILED) {
                server.send(401, "text/plain", "Connection failed. Wrong password or SSID.");
            } else {
                server.send(500, "text/plain", "Connection failed. Unknown error.");
            }
        }
    } else {
        server.send(400, "text/plain", "Missing SSID or password.");
    } });

  server.begin();
  Serial.println("HTTP server started in AP mode.");
}

connectToWiFi()
{
  WiFi.begin(client_ssid.c_str(), client_password.c_str());
  Serial.println("Connecting to WiFi...");

  unsigned long startAttemptTime = millis();

  while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 30000)
  {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED)
  {
    Serial.println("\nConnected to WiFi!");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());
    startMQTT();
  }
  else
  {
    Serial.println("\nFailed to connect to WiFi. Restarting...");
    return 440;
  }
}

void startMQTT()
{
  client.setServer(default_mqtt_broker, default_mqtt_port);
  client.setCallback([](char *topic, byte *payload, unsigned int length)
                     {
    Serial.print("Message arrived on topic: ");
    Serial.println(topic); });

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

void setup()
{
  Serial.begin(115200);

  pinMode(digitalPort, INPUT);
  pinMode(analogPort, INPUT);

  // Start in AP mode
  startAP();
}

void loop()
{
  if (WiFi.status() == WL_CONNECTED)
  {
    client.loop();

    StaticJsonDocument<200> data;
    int portValue = digitalRead(digitalPort);
    float soil = analogRead(analogPort);
    float temperature, humidity;

    if (dht_sensor.measure(&temperature, &humidity))
    {
      data["temp"] = temperature;
      data["humidity"] = humidity;
      data["soil"] = soil;
      data["light"] = (portValue == 0);

      char payload[100];
      serializeJson(data, payload, sizeof(payload));

      String topicString = "/sensors/" + WiFi.macAddress();
      client.publish(topicString.c_str(), payload);
    }

    delay(5000);
  }
  else
  {
    server.handleClient();
  }
}
