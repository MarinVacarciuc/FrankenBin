// SmartBin v0.3 — ESP8266 Telegram bot controller.
// Improved STATUS: includes fill %, temperature, humidity from Arduino.
// Parses Arduino serial events: FILL:%, TEMP:, HUM:, LID:*, ALARM:*.

#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include "secrets.h"

WiFiClientSecure secured_client;
UniversalTelegramBot bot(BOT_TOKEN, secured_client);

bool isLocked     = false;
int  fillPercent  = 0;
float temperature = 0.0;
float humidity    = 0.0;
String lidState   = "unknown";
String alarmState = "none";

unsigned long lastBotCheck = 0;
const unsigned long BOT_MTBS = 1000;

String serialBuffer = "";

void parseArduino(String msg) {
  msg.trim();
  if (msg.startsWith("FILL:"))   fillPercent = msg.substring(5).toInt();
  else if (msg.startsWith("TEMP:")) temperature = msg.substring(5).toFloat();
  else if (msg.startsWith("HUM:"))  humidity    = msg.substring(4).toFloat();
  else if (msg.startsWith("LID:"))  lidState    = msg.substring(4);
  else if (msg.startsWith("ALARM:")) {
    alarmState = msg.substring(6);
    if (alarmState == "FULL") isLocked = true;
    bot.sendMessage(CHAT_ID, "ALERT: Bin is full. Please empty the bin.", "");
  }
}

void handleNewMessages(int n) {
  for (int i = 0; i < n; i++) {
    String chat_id = bot.messages[i].chat_id;
    String text    = bot.messages[i].text;

    if (text == "/start") {
      String kb = "[[\"OPEN\",\"CLOSE\"],[\"LOCK\",\"UNLOCK\"],[\"STATUS\"]]";
      bot.sendMessageWithReplyKeyboard(chat_id, "SmartBin Control Panel", "", kb, true);
    }
    else if (text == "OPEN") {
      if (isLocked) { bot.sendMessage(chat_id, "Bin is locked or full.", ""); }
      else { Serial.write('o'); bot.sendMessage(chat_id, "Opening...", ""); }
    }
    else if (text == "CLOSE") {
      Serial.write('c');
      bot.sendMessage(chat_id, "Closing...", "");
    }
    else if (text == "LOCK") {
      isLocked = true;
      Serial.write('c');
      bot.sendMessage(chat_id, "Bin locked.", "");
    }
    else if (text == "UNLOCK") {
      isLocked = false;
      alarmState = "none";
      bot.sendMessage(chat_id, "Bin unlocked.", "");
    }
    else if (text == "STATUS") {
      String status = "";
      status += "Lid: "         + lidState + "\n";
      status += "Fill: "        + String(fillPercent) + "%\n";
      status += "Temperature: " + String(temperature, 1) + " C\n";
      status += "Humidity: "    + String(humidity, 1)    + "%\n";
      status += "WiFi signal: " + String(WiFi.RSSI())    + " dBm\n";
      status += "Lock: "        + String(isLocked ? "LOCKED" : "ACTIVE");
      bot.sendMessage(chat_id, status, "");
    }
  }
}

void setup() {
  Serial.begin(9600);
  secured_client.setInsecure();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) delay(500);
  bot.sendMessage(CHAT_ID, "SmartBin is online.", "");
}

void loop() {
  while (Serial.available() > 0) {
    char c = Serial.read();
    if (c == '\n') { parseArduino(serialBuffer); serialBuffer = ""; }
    else serialBuffer += c;
  }

  if (millis() - lastBotCheck > BOT_MTBS) {
    int n = bot.getUpdates(bot.last_message_received + 1);
    while (n) { handleNewMessages(n); n = bot.getUpdates(bot.last_message_received + 1); }
    lastBotCheck = millis();
  }
}
