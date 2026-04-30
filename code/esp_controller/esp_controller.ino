// SmartBin v0.1 — ESP8266 Telegram bot controller.
// Forwards user commands to Arduino via hardware Serial (115200 baud).
// Credentials live in secrets.h (not committed).

#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include "secrets.h"

WiFiClientSecure secured_client;
UniversalTelegramBot bot(BOT_TOKEN, secured_client);

bool isLocked = false;

unsigned long lastBotCheck = 0;
const unsigned long BOT_MTBS = 1000;

void handleNewMessages(int n) {
  for (int i = 0; i < n; i++) {
    String chat_id = bot.messages[i].chat_id;
    String text    = bot.messages[i].text;

    if (text == "/start") {
      String kb = "[[\"OPEN\",\"CLOSE\"],[\"LOCK\",\"UNLOCK\"],[\"STATUS\"]]";
      bot.sendMessageWithReplyKeyboard(chat_id, "SmartBin Control Panel", "", kb, true);
    }
    else if (text == "OPEN") {
      if (isLocked) { bot.sendMessage(chat_id, "System is locked.", ""); }
      else { Serial.println("OPEN_BIN"); bot.sendMessage(chat_id, "Opening...", ""); }
    }
    else if (text == "CLOSE") {
      Serial.println("CLOSE_BIN");
      bot.sendMessage(chat_id, "Closing...", "");
    }
    else if (text == "LOCK") {
      isLocked = true;
      Serial.println("CLOSE_BIN");
      bot.sendMessage(chat_id, "Bin locked.", "");
    }
    else if (text == "UNLOCK") {
      isLocked = false;
      bot.sendMessage(chat_id, "Bin unlocked.", "");
    }
    else if (text == "STATUS") {
      String status = "WiFi signal: " + String(WiFi.RSSI()) + " dBm\n";
      status += "Lock: " + String(isLocked ? "LOCKED" : "ACTIVE");
      bot.sendMessage(chat_id, status, "");
    }
  }
}

void setup() {
  Serial.begin(115200);
  secured_client.setInsecure();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) delay(500);
  bot.sendMessage(CHAT_ID, "SmartBin is online.", "");
}

void loop() {
  if (millis() - lastBotCheck > BOT_MTBS) {
    int n = bot.getUpdates(bot.last_message_received + 1);
    while (n) { handleNewMessages(n); n = bot.getUpdates(bot.last_message_received + 1); }
    lastBotCheck = millis();
  }
}
