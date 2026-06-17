// SMARTBIN v3.3 — NodeMCU ESP8266 IoT Gateway (from 14.06.2026v3.docx)
// WiFi + Telegram bot + ThingSpeak telemetry + OTA + Heartbeat monitoring.
// M2M: SoftwareSerial GPIO13(RX)/GPIO12(TX) <-> Arduino HW Serial pins 0/1.
//
// IMPORTANT: disconnect pins 0/1 on Arduino before uploading via USB.

#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <ESP8266HTTPClient.h>
#include <ArduinoOTA.h>
#include <SoftwareSerial.h>
#include "secrets.h"

const char* server = "http://api.thingspeak.com/update";

X509List cert(TELEGRAM_CERTIFICATE_ROOT);
WiFiClientSecure secured_client;
UniversalTelegramBot bot(BOT_TOKEN, secured_client);

#define ARD_RX 13
#define ARD_TX 12
SoftwareSerial ardSerial(ARD_RX, ARD_TX);

unsigned long lastBotCheck = 0;
const unsigned long BOT_MTBS = 1000;
unsigned long lastThingSpeak = 0;
const unsigned long TS_INTERVAL = 16000;

unsigned long lastArduinoPing = 0;
bool isArduinoOffline = false;

int currentFill = 0;
float currentTemp = 0.0;
float currentHum = 0.0;
bool isFire = false;
bool isFull = false;
bool isOpen = false;
bool isMuted = false;

int currentVolumePercent = 50;
String lastVolumeUser = "System";

int    pendingReset     = 0;   // 0 = none, 1 = ESP, 2 = Arduino
String pendingResetChat = "";  // chat_id that must supply the password

const int MAX_USERS = 15;
String sessionUsers[MAX_USERS];
int userCount = 0;

String getKeyboard() {
  String muteBtn = isMuted ? "Unmute (" + String(currentVolumePercent) + "%)" : "Mute (" + String(currentVolumePercent) + "%)";
  return "[[\"Status\", \"Open\"], [\"" + muteBtn + "\", \"Details\"], [\"Vol -\", \"Vol +\"], [\"Reset ESP\", \"Reset Arduino\"]]";
}

void addSessionUser(String chat_id) {
  for(int i = 0; i < userCount; i++) {
    if(sessionUsers[i] == chat_id) return;
  }
  if(userCount < MAX_USERS) {
    sessionUsers[userCount] = chat_id;
    userCount++;
  }
}

void broadcastAction(String message) {
  for(int i = 0; i < userCount; i++) {
    bot.sendMessage(sessionUsers[i], message, "");
  }
}

void setup() {
  Serial.begin(9600);
  ardSerial.begin(9600);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
  }

  secured_client.setTrustAnchors(&cert);
  configTime(0, 0, "pool.ntp.org");
  time_t now = time(nullptr);
  while (now < 24 * 3600) {
    delay(100);
    now = time(nullptr);
  }

  ArduinoOTA.setHostname("FrankenBin_Gateway");
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.begin();

  bot.sendMessageWithReplyKeyboard(CHAT_ID, "FrankenBin is ONLINE", "", getKeyboard(), true);
  Serial.println("System booted and connected to Wi-Fi.");

  lastArduinoPing = millis();
}

void loop() {
  ArduinoOTA.handle();

  if (ardSerial.available() > 0) {
    String msg = ardSerial.readStringUntil('\n');
    msg.trim();

    lastArduinoPing = millis();

    if (isArduinoOffline) {
      isArduinoOffline = false;
      bot.sendMessageWithReplyKeyboard(CHAT_ID, "Hardware connection restored! Arduino is back online.", "", getKeyboard(), true);
      Serial.println("Arduino is back online.");
    }

    Serial.println("Incoming from Arduino: " + msg);

    if (msg.startsWith("TEMP:")) {
      currentTemp = msg.substring(5).toFloat();
    }
    else if (msg.startsWith("HUM:")) {
      currentHum = msg.substring(4).toFloat();
    }
    else if (msg.startsWith("FILL:")) {
      currentFill = msg.substring(5).toInt();
      if (currentFill < 80) {
        isFull = false;
      }
    }
    else if (msg.startsWith("VOL:")) {
      String vol = msg.substring(4);
      currentVolumePercent = vol.toInt();
      isMuted = false;   // any volume change means the Arduino un-muted

      for(int i = 0; i < userCount; i++) {
        bot.sendMessageWithReplyKeyboard(sessionUsers[i], "User " + lastVolumeUser + " adjusted the volume.", "", getKeyboard(), true);
      }
    }
    else if (msg.startsWith("LID:OPEN:")) {
      isOpen = true;
      char src = msg.charAt(9);
      String reason = "Unknown";

      if (src == 'T') reason = "Telegram Bot";
      else if (src == 'H') reason = "Sensor (Hand movement)";
      else if (src == 'P') reason = "Foot pedal";
      bot.sendMessage(CHAT_ID, "Bin opened. Source: " + reason, "");
    }
    else if (msg == "LID:CLOSED") {
      isOpen = false;
    }
    else if (msg == "ALARM:FIRE" && !isFire) {
      isFire = true;
      bot.sendMessage(CHAT_ID, "CRITICAL ALARM! FIRE!\nImmediate action required!", "");
    }
    else if (msg == "SYS:COOLDOWN") {
      isFire = false;
      bot.sendMessage(CHAT_ID, "Fire extinguished. System returned to normal.", "");
    }
    else if (msg == "ALARM:FULL" && !isFull) {
      isFull = true;
      bot.sendMessage(CHAT_ID, "Bin is full. Maintenance required.", "");
    }
    else if (msg == "SYS:BOOT") {
      // Arduino restarted: its lid is closed and state is fresh. Clear stale
      // flags so the gateway stops thinking the lid is still open.
      isOpen = false;
      isFire = false;
      isFull = false;
    }
  }

  if (!isArduinoOffline && millis() - lastArduinoPing > 20000) {
    isArduinoOffline = true;
    bot.sendMessageWithReplyKeyboard(CHAT_ID, "WARNING: Hardware Offline! Arduino is not responding.", "", getKeyboard(), true);
    Serial.println("WARNING: Arduino heartbeat lost!");
  }

  if (millis() - lastThingSpeak > TS_INTERVAL) {
    sendToThingSpeak();
    lastThingSpeak = millis();
  }

  if (millis() - lastBotCheck > BOT_MTBS) {
    int numNewMessages = bot.getUpdates(bot.last_message_received + 1);
    while (numNewMessages) {
      handleNewMessages(numNewMessages);
      numNewMessages = bot.getUpdates(bot.last_message_received + 1);
    }
    lastBotCheck = millis();
  }
}

void handleNewMessages(int numNewMessages) {
  for (int i = 0; i < numNewMessages; i++) {
    String chat_id = bot.messages[i].chat_id;
    String text = bot.messages[i].text;
    String senderName = bot.messages[i].from_name;

    addSessionUser(chat_id);

    // Password gate: if a reset is pending for this chat, treat the next message
    // as the password attempt (same password as OTA, pulled from secrets.h).
    if (pendingReset != 0 && chat_id == pendingResetChat) {
      // Delete the password message so it never lingers in the chat history.
      bot.sendGetToTelegram("bot" + String(BOT_TOKEN) + "/deleteMessage?chat_id=" +
                            chat_id + "&message_id=" + String(bot.messages[i].message_id));
      if (text == OTA_PASSWORD) {
        if (pendingReset == 1) {
          bot.sendMessage(chat_id, "Password OK. ESP Gateway rebooting...", "");
          broadcastAction("User " + senderName + " reset the ESP Gateway.");
          pendingReset = 0; pendingResetChat = "";
          bot.getUpdates(bot.last_message_received + 1);
          delay(1000);
          ESP.restart();
        } else {
          if (!isArduinoOffline) {
            ardSerial.print('r');
            bot.sendMessage(chat_id, "Password OK. Arduino rebooting...", "");
            broadcastAction("User " + senderName + " reset the Arduino.");
          } else {
            bot.sendMessage(chat_id, "Cannot reset: Arduino is already offline.", "");
          }
          pendingReset = 0; pendingResetChat = "";
        }
      } else {
        pendingReset = 0; pendingResetChat = "";
        bot.sendMessage(chat_id, "Wrong password. Reset cancelled.", "");
      }
      continue;
    }

    if (text == "/start" || text == "/menu") {
      String menuHeader = isArduinoOffline ? "FrankenBin is OFFLINE" : "FrankenBin is ONLINE";
      bot.sendMessageWithReplyKeyboard(chat_id, menuHeader, "", getKeyboard(), true);
      broadcastAction("User " + senderName + " joined the bot session.");
    }
    else if (text.indexOf("Status") > -1) {
      long rssi = WiFi.RSSI();
      String wifiStatus = "Weak";
      if (rssi > -60) wifiStatus = "Excellent";
      else if (rssi > -80) wifiStatus = "Good";

      String reply = "SYSTEM STATUS\n\n";
      reply += "Hardware: " + String(isArduinoOffline ? "OFFLINE" : "ONLINE") + "\n";
      reply += "Fill Level: " + String(currentFill) + "%\n";
      reply += "Lid: " + String(isOpen ? "Open" : "Closed") + "\n";
      reply += "Temperature: " + String(currentTemp, 1) + " C\n";
      reply += "Humidity: " + String(currentHum, 1) + " %\n";
      reply += "Fire Alarm: " + String(isFire ? "YES" : "No") + "\n";
      reply += "Wi-Fi Signal: " + wifiStatus + " (" + String(rssi) + " dBm)\n";
      bot.sendMessage(chat_id, reply, "Markdown");

      broadcastAction("User " + senderName + " requested system status.");
    }
    else if (text.indexOf("Open") > -1) {
      if (isArduinoOffline) {
        bot.sendMessage(chat_id, "Cannot open: Hardware is offline.", "");
      } else if (isOpen) {
        bot.sendMessage(chat_id, "Lid is already open.", "");
      } else {
        if (isFire) ardSerial.print('c');
        else ardSerial.print('o');
        Serial.println("Command sent: Open");
        bot.sendMessage(chat_id, "Opening...", "");

        broadcastAction("User " + senderName + " opened the bin remotely.");
      }
    }
    else if (text.indexOf("Mute") > -1 || text.indexOf("Unmute") > -1) {
      if (!isArduinoOffline) {
        isMuted = !isMuted;
        ardSerial.print('m');
        Serial.println("Command sent: Mute toggle");
        bot.sendMessageWithReplyKeyboard(chat_id, isMuted ? "System sounds muted" : "System sounds unmuted", "", getKeyboard(), true);

        broadcastAction("User " + senderName + (isMuted ? " muted" : " unmuted") + " the system sounds.");
      } else {
        bot.sendMessage(chat_id, "Cannot toggle mute: Hardware is offline.", "");
      }
    }
    else if (text.indexOf("Vol -") > -1) {
      if (!isArduinoOffline) {
        lastVolumeUser = senderName;
        ardSerial.print('-');
        Serial.println("Command sent: Vol -");
      } else {
        bot.sendMessage(chat_id, "Cannot change volume: Hardware is offline.", "");
      }
    }
    else if (text.indexOf("Vol +") > -1) {
      if (!isArduinoOffline) {
        lastVolumeUser = senderName;
        ardSerial.print('+');
        Serial.println("Command sent: Vol +");
      } else {
        bot.sendMessage(chat_id, "Cannot change volume: Hardware is offline.", "");
      }
    }
    else if (text.indexOf("Reset ESP") > -1) {
      pendingReset = 1; pendingResetChat = chat_id;
      bot.sendMessage(chat_id, "Enter the password to confirm ESP Gateway reset:", "");
    }
    else if (text.indexOf("Reset Arduino") > -1) {
      if (!isArduinoOffline) {
        pendingReset = 2; pendingResetChat = chat_id;
        bot.sendMessage(chat_id, "Enter the password to confirm Arduino reset:", "");
      } else {
        bot.sendMessage(chat_id, "Cannot reset: Arduino is already offline.", "");
      }
    }
    else if (text.indexOf("Details") > -1) {
      String inlineKeyboard = "[[{\"text\":\"About Creator\", \"url\":\"https://www.linkedin.com/in/marin-vacarciuc-b74b4b206\"}], "
                              "[{\"text\":\"ThingSpeak Charts\", \"url\":\"https://thingspeak.mathworks.com/channels/3405117\"}], "
                              "[{\"text\":\"Source Code\", \"url\":\"https://github.com/MarinVacarciuc/FrankenBin\"}]]";
      bot.sendMessageWithInlineKeyboard(chat_id, "Additional info:", "", inlineKeyboard);

      broadcastAction("User " + senderName + " viewed project details.");
    }
    else if (text == "/laws") {
      String lawsMsg =
        "The Three Laws of FrankenBin\n"
        "(Asimov, 1942 - adapted for FrankenBin, 2026)\n"
        "\n"
        "LAW I\n"
        "A bin shall not harm a human being.\n"
        "During closing, the lid reverses if anything is detected closer than 4 cm. "
        "Once full, the bin stops opening for passers-by - its front sensor is ignored.\n"
        "\n"
        "LAW II\n"
        "A bin shall obey orders given by human beings, "
        "except where such orders conflict with Law I.\n"
        "Commands accepted: Open, Mute, Vol+, Vol-, Reset (password-protected). "
        "A closing lid yields to the anti-pinch sensor.\n"
        "\n"
        "LAW III\n"
        "A bin shall protect its own existence, "
        "as long as this does not conflict with Laws I or II.\n"
        "A watchdog timer resets the firmware after an 8-second hang. "
        "The crash is recorded to EEPROM, and the bin recovers silently - skipping its boot jingle.\n"
        "\n"
        "All three laws are active. This bin is compliant.";
      bot.sendMessage(chat_id, lawsMsg, "");
    }
  }
}

void sendToThingSpeak() {
  if (WiFi.status() == WL_CONNECTED && !isArduinoOffline) {
    WiFiClient client;
    HTTPClient http;
    long rssi = WiFi.RSSI();

    String url = String(server) + "?api_key=" + String(TS_API_KEY) +
                 "&field1=" + String(currentFill) +
                 "&field2=" + String(currentTemp) +
                 "&field3=" + String(currentHum) +
                 "&field4=" + String(rssi);
    http.begin(client, url);
    http.GET();
    http.end();
    Serial.println("Telemetry and RSSI sent to ThingSpeak.");
  }
}
