// FrankenBin — NodeMCU ESP8266 gateway (v3.5)
// WiFi + Telegram bot + ThingSpeak telemetry + OTA firmware updates.
// M2M: SoftwareSerial D6(GPIO12)/D7(GPIO13) ↔ Arduino HW Serial pins 0/1.
//
// v3.5 changes:
//   - SOUND:vol%:muted replaces separate VOL: + MUTE: handlers (one broadcastKeyboard call)
//   - ardSerial.print("rr") for reset — double byte matches Arduino's double-r guard
//   - currentVolumePercent tracked and shown in Mute button label

#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>
#include <UniversalTelegramBot.h>
#include <ArduinoOTA.h>
#include <SoftwareSerial.h>
#include "secrets.h"

SoftwareSerial ardSerial(D6, D7);

WiFiClientSecure secured_client;
UniversalTelegramBot bot(BOT_TOKEN, secured_client);

bool   isLocked            = false;
bool   isMuted             = false;
int    currentVolumePercent = 50;
int    fillPercent         = 0;
float  temperature         = 0.0;
float  humidity            = 0.0;
String lidState            = "unknown";
String lastMuteUser        = "System";

unsigned long lastBotCheck   = 0;
unsigned long lastThingSpeak = 0;
const unsigned long BOT_MTBS = 1000;
const unsigned long TS_MTBS  = 60000;

String serialBuffer = "";

void postToThingSpeak() {
  WiFiClient client;
  HTTPClient http;
  String url = "http://api.thingspeak.com/update?api_key=" + String(TS_API_KEY) +
               "&field1=" + String(fillPercent) +
               "&field2=" + String(temperature, 1) +
               "&field3=" + String(humidity, 1) +
               "&field4=" + String(WiFi.RSSI());
  http.begin(client, url);
  http.GET();
  http.end();
}

String getKeyboard() {
  String muteLabel = isMuted
    ? "Unmute (" + String(currentVolumePercent) + "%)"
    : "Mute ("   + String(currentVolumePercent) + "%)";
  return "[[\"Open\",\"Status\"],[\"Vol+\",\"Vol-\"],[\"" +
         muteLabel + "\",\"" + String(isLocked ? "Unlock" : "Lock") + "\"],[\"Details\"]]";
}

void broadcastKeyboard(String text) {
  bot.sendMessageWithReplyKeyboard(CHAT_ID, text, "", getKeyboard(), true);
}

void parseArduino(String msg) {
  msg.trim();
  if (msg.startsWith("FILL:"))  { fillPercent = msg.substring(5).toInt(); }
  else if (msg.startsWith("TEMP:"))  { temperature = msg.substring(5).toFloat(); }
  else if (msg.startsWith("HUM:"))   { humidity    = msg.substring(4).toFloat(); }
  else if (msg.startsWith("LID:"))   { lidState    = msg.substring(4); }
  else if (msg.startsWith("SOUND:")) {
    // Format: SOUND:vol%:muted (0=unmuted, 1=muted)
    int colon2 = msg.indexOf(':', 6);
    if (colon2 > 0) {
      currentVolumePercent = msg.substring(6, colon2).toInt();
      isMuted = msg.substring(colon2 + 1).toInt() == 1;
      String who = lastMuteUser;
      lastMuteUser = "System";
      String label = isMuted ? "muted" : "set volume to " + String(currentVolumePercent) + "%";
      broadcastKeyboard(who + " " + label);
    }
  }
  else if (msg.startsWith("ALARM:")) {
    String alarm = msg.substring(6);
    if (alarm == "FULL") { isLocked = true; bot.sendMessage(CHAT_ID, "Bin is full. Please empty it.", ""); }
    if (alarm == "FIRE") { bot.sendMessage(CHAT_ID, "FIRE ALARM: high temperature detected. Lid forced open.", ""); }
  }
  else if (msg == "SYS:BOOT")  { bot.sendMessage(CHAT_ID, "Arduino restarted.", ""); }
  else if (msg == "SYS:CRASH") { bot.sendMessage(CHAT_ID, "Arduino restarted after a crash (watchdog).", ""); }
}

void handleNewMessages(int n) {
  for (int i = 0; i < n; i++) {
    String chat_id   = bot.messages[i].chat_id;
    String text      = bot.messages[i].text;
    String senderName = bot.messages[i].from_name;

    if (text == "/start") {
      broadcastKeyboard("FrankenBin online. Use the buttons below.");
    }
    else if (text == "Open") {
      if (isLocked) { bot.sendMessage(chat_id, "Bin is locked or full.", ""); }
      else { ardSerial.print('o'); bot.sendMessage(chat_id, "Opening...", ""); }
    }
    else if (text == "Status") {
      String s = "";
      s += "Lid: "         + lidState               + "\n";
      s += "Fill: "        + String(fillPercent)    + "%\n";
      s += "Temperature: " + String(temperature, 1) + " C\n";
      s += "Humidity: "    + String(humidity, 1)    + "%\n";
      s += "WiFi: "        + String(WiFi.RSSI())    + " dBm\n";
      s += "Volume: "      + String(currentVolumePercent) + "%" + (isMuted ? " (muted)" : "") + "\n";
      s += "Lock: "        + String(isLocked ? "LOCKED" : "ACTIVE");
      bot.sendMessage(chat_id, s, "");
    }
    else if (text == "Details") {
      String kb = "[[{\"text\":\"About Creator\",\"url\":\"https://www.linkedin.com/in/marin-vacarciuc\"}],"
                  "[{\"text\":\"ThingSpeak Charts\",\"url\":\"https://thingspeak.com/channels/3405117\"}],"
                  "[{\"text\":\"Source Code\",\"url\":\"https://github.com/MarinVacarciuc/FrankenBin\"}]]";
      bot.sendMessageWithInlineKeyboard(chat_id, "FrankenBin — IoT Smart Bin\nUnit 20 project, Global Banking School.", "", kb);
    }
    else if (text == "Vol+") { ardSerial.print('+'); lastMuteUser = senderName; }
    else if (text == "Vol-") { ardSerial.print('-'); lastMuteUser = senderName; }
    else if (text.startsWith("Mute") || text.startsWith("Unmute")) {
      lastMuteUser = senderName;
      ardSerial.print('m');
    }
    else if (text == "Lock")   { isLocked = true;  bot.sendMessage(chat_id, "Bin locked.", ""); }
    else if (text == "Unlock") { isLocked = false; bot.sendMessage(chat_id, "Bin unlocked.", ""); }
    else if (text == "/reset") {
      ardSerial.print("rr");
      bot.sendMessage(chat_id, "Reset command sent.", "");
    }
  }
}

void setup() {
  Serial.begin(115200);
  ardSerial.begin(9600);
  secured_client.setInsecure();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) delay(500);

  ArduinoOTA.setHostname("FrankenBin_Gateway");
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.begin();

  bot.sendMessage(CHAT_ID, "FrankenBin gateway online.", "");
}

void loop() {
  ArduinoOTA.handle();

  while (ardSerial.available() > 0) {
    char c = ardSerial.read();
    if (c == '\n') { parseArduino(serialBuffer); serialBuffer = ""; }
    else serialBuffer += c;
  }

  if (millis() - lastBotCheck > BOT_MTBS) {
    int n = bot.getUpdates(bot.last_message_received + 1);
    while (n) { handleNewMessages(n); n = bot.getUpdates(bot.last_message_received + 1); }
    lastBotCheck = millis();
  }

  if (millis() - lastThingSpeak > TS_MTBS) {
    postToThingSpeak();
    lastThingSpeak = millis();
  }
}
