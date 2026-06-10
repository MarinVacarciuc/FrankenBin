// SmartBin — NodeMCU ESP8266 gateway (v1.0)
// Handles all networking: Telegram bot, WiFi.
// Talks to Arduino Uno via SoftwareSerial D6(GPIO12)/D7(GPIO13) at 9600 baud.
// Receives: FILL:%, TEMP:, HUM:, LID:*, ALARM:*, VOL:%, MUTE:*
// Sends:    'o' open, 'm' mute, '+'/'-' volume, 'r' reset

#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <SoftwareSerial.h>
#include "secrets.h"

SoftwareSerial ardSerial(D6, D7);

WiFiClientSecure secured_client;
UniversalTelegramBot bot(BOT_TOKEN, secured_client);

bool   isLocked    = false;
bool   isMuted     = false;
int    fillPercent = 0;
float  temperature = 0.0;
float  humidity    = 0.0;
String lidState    = "unknown";

unsigned long lastBotCheck = 0;
const unsigned long BOT_MTBS = 1000;

String serialBuffer = "";

String getKeyboard() {
  return "[[\"Open\",\"Status\"],[\"Vol+\",\"Vol-\"],[\"" +
         String(isMuted ? "Unmute" : "Mute") +
         "\",\"" + String(isLocked ? "Unlock" : "Lock") + "\"]]";
}

void refreshKeyboardForAll(String text) {
  bot.sendMessageWithReplyKeyboard(CHAT_ID, text, "", getKeyboard(), true);
}

void parseArduino(String msg) {
  msg.trim();
  if      (msg.startsWith("FILL:"))  { fillPercent = msg.substring(5).toInt(); }
  else if (msg.startsWith("TEMP:"))  { temperature = msg.substring(5).toFloat(); }
  else if (msg.startsWith("HUM:"))   { humidity    = msg.substring(4).toFloat(); }
  else if (msg.startsWith("LID:"))   { lidState    = msg.substring(4); }
  else if (msg.startsWith("VOL:"))   {
    refreshKeyboardForAll("Volume set to " + msg.substring(4) + "%");
  }
  else if (msg.startsWith("MUTE:"))  {
    isMuted = msg.substring(5) == "ON";
    refreshKeyboardForAll(isMuted ? "Muted." : "Unmuted.");
  }
  else if (msg.startsWith("ALARM:")) {
    String alarm = msg.substring(6);
    if (alarm == "FULL") { isLocked = true; bot.sendMessage(CHAT_ID, "Bin is full. Please empty it.", ""); }
  }
  else if (msg == "SYS:BOOT")  { bot.sendMessage(CHAT_ID, "Arduino restarted.", ""); }
  else if (msg == "SYS:CRASH") { bot.sendMessage(CHAT_ID, "Arduino restarted after a crash (watchdog).", ""); }
}

void handleNewMessages(int n) {
  for (int i = 0; i < n; i++) {
    String chat_id = bot.messages[i].chat_id;
    String text    = bot.messages[i].text;

    if (text == "/start") {
      refreshKeyboardForAll("SmartBin online. Use the buttons below.");
    }
    else if (text == "Open") {
      if (isLocked) { bot.sendMessage(chat_id, "Bin is locked or full.", ""); }
      else { ardSerial.print('o'); bot.sendMessage(chat_id, "Opening...", ""); }
    }
    else if (text == "Status") {
      String s = "";
      s += "Lid: "         + lidState             + "\n";
      s += "Fill: "        + String(fillPercent)  + "%\n";
      s += "Temperature: " + String(temperature, 1) + " C\n";
      s += "Humidity: "    + String(humidity, 1)  + "%\n";
      s += "WiFi: "        + String(WiFi.RSSI())  + " dBm\n";
      s += "Muted: "       + String(isMuted ? "yes" : "no") + "\n";
      s += "Lock: "        + String(isLocked ? "LOCKED" : "ACTIVE");
      bot.sendMessage(chat_id, s, "");
    }
    else if (text == "Vol+")   { ardSerial.print('+'); }
    else if (text == "Vol-")   { ardSerial.print('-'); }
    else if (text == "Mute" || text == "Unmute") {
      ardSerial.print('m');
    }
    else if (text == "Lock") {
      isLocked = true;
      bot.sendMessage(chat_id, "Bin locked.", "");
    }
    else if (text == "Unlock") {
      isLocked = false;
      bot.sendMessage(chat_id, "Bin unlocked.", "");
    }
  }
}

void setup() {
  Serial.begin(115200);
  ardSerial.begin(9600);
  secured_client.setInsecure();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) delay(500);
  bot.sendMessage(CHAT_ID, "SmartBin gateway online.", "");
}

void loop() {
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
}
