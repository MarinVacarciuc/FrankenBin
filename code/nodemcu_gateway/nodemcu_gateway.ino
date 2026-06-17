// FrankenBin — NodeMCU ESP8266 gateway (v3.6)
// WiFi + Telegram bot + ThingSpeak telemetry + OTA firmware updates.
// M2M: SoftwareSerial D6(GPIO12)=RX / D7(GPIO13)=TX ↔ Arduino HW Serial 0/1.
//
// Receives from Arduino: FILL:, TEMP:, HUM:, LID:, SOUND:, ALARM:, SYS:
// Sends to Arduino (single chars): 'o' 'm' '+' '-'  ("rr" for reset)

#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>
#include <UniversalTelegramBot.h>
#include <ArduinoOTA.h>
#include <SoftwareSerial.h>
#include "secrets.h"

// --- M2M serial (Arduino HW Serial 0/1, cross-wired) ------------------------
SoftwareSerial ardSerial(D6, D7);   // D6=RX ← Arduino TX(1), D7=TX → Arduino RX(0)

// --- Telegram ----------------------------------------------------------------
WiFiClientSecure  secured_client;
UniversalTelegramBot bot(BOT_TOKEN, secured_client);

// --- Runtime state (mirrors Arduino; authoritative source is Arduino) --------
bool   isLocked            = false;
bool   isMuted             = false;
int    currentVolumePercent = 50;
int    fillPercent         = 0;
float  temperature         = 0.0;
float  humidity            = 0.0;
String lidState            = "unknown";
String lastActionUser      = "System";  // who triggered the last vol/mute action

// --- Timers ------------------------------------------------------------------
unsigned long lastBotCheck   = 0;
unsigned long lastThingSpeak = 0;
const unsigned long BOT_MTBS = 1000;
const unsigned long TS_MTBS  = 60000;

// --- Serial buffer -----------------------------------------------------------
String serialBuffer = "";
const size_t SERIAL_BUF_MAX = 64;

// --- ThingSpeak --------------------------------------------------------------
void postToThingSpeak() {
  if (WiFi.status() != WL_CONNECTED) return;
  WiFiClient client;
  HTTPClient http;
  String url = "http://api.thingspeak.com/update?api_key=" + String(TS_API_KEY)
             + "&field1=" + String(fillPercent)
             + "&field2=" + String(temperature, 1)
             + "&field3=" + String(humidity, 1)
             + "&field4=" + String(WiFi.RSSI());
  http.begin(client, url);
  int code = http.GET();
  if (code <= 0) { /* silent fail — ThingSpeak is non-critical */ }
  http.end();
}

// --- Telegram keyboard -------------------------------------------------------
String getKeyboard() {
  String muteLabel = isMuted
    ? "Unmute (" + String(currentVolumePercent) + "%)"
    : "Mute ("   + String(currentVolumePercent) + "%)";
  String lockLabel = isLocked ? "Unlock" : "Lock";
  return "[[\"Open\",\"Status\"],[\"Vol+\",\"Vol-\"],[\""
       + muteLabel + "\",\"" + lockLabel + "\"],[\"Details\"]]";
}

void broadcastKeyboard(const String& text) {
  bot.sendMessageWithReplyKeyboard(CHAT_ID, text, "", getKeyboard(), true);
}

// --- Arduino serial parser ---------------------------------------------------
void parseArduino(const String& raw) {
  String msg = raw;
  msg.trim();
  if (msg.length() == 0) return;

  if (msg.startsWith("FILL:")) {
    fillPercent = msg.substring(5).toInt();

  } else if (msg.startsWith("TEMP:")) {
    temperature = msg.substring(5).toFloat();

  } else if (msg.startsWith("HUM:")) {
    humidity = msg.substring(4).toFloat();

  } else if (msg.startsWith("LID:")) {
    lidState = msg.substring(4);

  } else if (msg.startsWith("SOUND:")) {
    // Format: SOUND:<vol%>:<muted>   e.g. SOUND:50:0
    int sep = msg.indexOf(':', 6);
    if (sep < 0) return;
    int  newVol    = msg.substring(6, sep).toInt();
    bool newMuted  = (msg.substring(sep + 1).toInt() == 1);

    String label;
    if (newMuted != isMuted) {
      label = newMuted ? "muted" : "unmuted";            // toggle event
    } else {
      label = "set volume to " + String(newVol) + "%";   // vol+/vol- event
    }

    currentVolumePercent = newVol;
    isMuted              = newMuted;

    String who = lastActionUser;
    lastActionUser = "System";
    broadcastKeyboard(who + " " + label);

  } else if (msg.startsWith("ALARM:")) {
    String alarm = msg.substring(6);
    if (alarm == "FULL") {
      isLocked = true;
      bot.sendMessage(CHAT_ID, "Bin is full — please empty it.", "");
    } else if (alarm == "FIRE") {
      bot.sendMessage(CHAT_ID, "FIRE ALARM: high temperature detected. Lid forced open.", "");
    }

  } else if (msg == "SYS:BOOT") {
    bot.sendMessage(CHAT_ID, "Arduino restarted.", "");

  } else if (msg == "SYS:CRASH") {
    bot.sendMessage(CHAT_ID, "Arduino restarted after a watchdog crash.", "");
  }
}

// --- Telegram message handler ------------------------------------------------
void handleNewMessages(int n) {
  for (int i = 0; i < n; i++) {
    String chat_id    = bot.messages[i].chat_id;
    String text       = bot.messages[i].text;
    String senderName = bot.messages[i].from_name;

    if (text == "/start") {
      broadcastKeyboard("FrankenBin online. Use the buttons below.");

    } else if (text == "Open") {
      if (isLocked) {
        bot.sendMessage(chat_id, "Bin is locked or full. Unlock first.", "");
      } else {
        ardSerial.print('o');
        bot.sendMessage(chat_id, "Opening...", "");
      }

    } else if (text == "Status") {
      char buf[200];
      snprintf(buf, sizeof(buf),
        "Lid: %s\nFill: %d%%\nTemp: %.1f C\nHumidity: %.1f%%\n"
        "Volume: %d%%%s\nWiFi: %d dBm\nLock: %s",
        lidState.c_str(), fillPercent, temperature, humidity,
        currentVolumePercent, isMuted ? " (muted)" : "",
        WiFi.RSSI(), isLocked ? "LOCKED" : "ACTIVE");
      bot.sendMessage(chat_id, buf, "");

    } else if (text == "Details") {
      String kb = "[[{\"text\":\"About Creator\",\"url\":\"https://www.linkedin.com/in/marin-vacarciuc\"}],"
                   "[{\"text\":\"ThingSpeak Charts\",\"url\":\"https://thingspeak.com/channels/3405117\"}],"
                   "[{\"text\":\"Source Code\",\"url\":\"https://github.com/MarinVacarciuc/FrankenBin\"}]]";
      bot.sendMessageWithInlineKeyboard(chat_id,
        "FrankenBin — IoT Smart Bin\nUnit 20 project, Global Banking School.", "", kb);

    } else if (text == "Vol+") {
      lastActionUser = senderName;
      ardSerial.print('+');

    } else if (text == "Vol-") {
      lastActionUser = senderName;
      ardSerial.print('-');

    } else if (text.startsWith("Mute") || text.startsWith("Unmute")) {
      lastActionUser = senderName;
      ardSerial.print('m');
      // Do NOT pre-apply isMuted here — wait for Arduino's SOUND: confirmation.

    } else if (text == "Lock") {
      isLocked = true;
      bot.sendMessage(chat_id, "Bin locked.", "");

    } else if (text == "Unlock") {
      isLocked = false;
      bot.sendMessage(chat_id, "Bin unlocked.", "");

    } else if (text == "/reset") {
      if (chat_id == String(CHAT_ID)) {
        ardSerial.print("rr");
        bot.sendMessage(chat_id, "Reset command sent.", "");
      } else {
        bot.sendMessage(chat_id, "This command is owner-only.", "");
      }

    } else if (text == "/laws") {
      String msg =
        "The Three Laws of FrankenBin\n"
        "(Asimov, 1942 — adapted for FrankenBin, 2026)\n"
        "\n"
        "LAW I\n"
        "A bin shall not harm a human being.\n"
        "The lid reverses during closing if anything is detected closer than 4 cm. "
        "A full bin ignores the Open command — Law I overrides Law II.\n"
        "\n"
        "LAW II\n"
        "A bin shall obey orders given by human beings, "
        "except where such orders conflict with Law I.\n"
        "Commands accepted: Open, Mute, Vol+, Vol-, /reset.\n"
        "\n"
        "LAW III\n"
        "A bin shall protect its own existence, "
        "as long as this does not conflict with Laws I or II.\n"
        "A watchdog timer resets the firmware after a 4-second hang. "
        "The crash is recorded to EEPROM and reported here as SYS:CRASH.\n"
        "\n"
        "All three laws are active. This bin is compliant.";
      bot.sendMessage(chat_id, msg, "");
    }
  }
}

// --- WiFi reconnect ----------------------------------------------------------
void ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  WiFi.reconnect();
  unsigned long t = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t < 10000) delay(500);
}

// --- Setup -------------------------------------------------------------------
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

// --- Loop --------------------------------------------------------------------
void loop() {
  ArduinoOTA.handle();
  ensureWiFi();

  // Drain Arduino serial into line buffer
  while (ardSerial.available() > 0) {
    char c = ardSerial.read();
    if (c == '\n') {
      parseArduino(serialBuffer);
      serialBuffer = "";
    } else if (serialBuffer.length() < SERIAL_BUF_MAX) {
      serialBuffer += c;
    } else {
      serialBuffer = "";  // overflow: discard and start fresh
    }
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
