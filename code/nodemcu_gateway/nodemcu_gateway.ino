// ============================================================================
//  SMARTBIN (FrankenBin) — NodeMCU ESP8266 IoT GATEWAY  (v3.3 base)
// ----------------------------------------------------------------------------
//  ROLE OF THIS BOARD: the "messenger". The ESP8266 is the ONLY board with
//  Wi-Fi. It does NOT run any safety logic. Its jobs are:
//    1. Read the short text "events" the Arduino sends over the serial cable
//       (e.g. "FILL:73", "ALARM:FIRE") and turn them into Telegram alerts.
//    2. Push telemetry (fill, temperature, humidity, signal) to ThingSpeak.
//    3. Receive commands from the Telegram bot and forward single letters to
//       the Arduino (e.g. 'o' = open, 'm' = mute, 'r' = reset).
//    4. Allow remote firmware updates over Wi-Fi (OTA).
//    5. Watch the Arduino's "heartbeat" and warn if it goes silent.
//
//  WHY SPLIT THIS WAY: if Wi-Fi fails, only this board is affected — the bin
//  itself (lid, anti-pinch, fire) keeps working because all of that lives on
//  the Arduino. The cloud just degrades to "telemetry unavailable".
//
//  M2M LINK: SoftwareSerial GPIO13(RX)/GPIO12(TX) <-> Arduino HW Serial 0/1.
//  IMPORTANT: disconnect the Arduino's pins 0/1 before uploading to it by USB.
// ============================================================================

// ---- Libraries -------------------------------------------------------------
#include <ESP8266WiFi.h>            // Wi-Fi for the ESP8266
#include <WiFiClientSecure.h>       // TLS/HTTPS client (encrypted connection to Telegram)
#include <UniversalTelegramBot.h>   // Telegram Bot API wrapper
#include <ESP8266HTTPClient.h>      // simple HTTP client (used for ThingSpeak)
#include <ArduinoOTA.h>             // over-the-air firmware updates
#include <SoftwareSerial.h>         // extra serial port (the M2M link to the Arduino)
#include "secrets.h"               // Wi-Fi/Telegram/ThingSpeak credentials (NOT in git)

// ThingSpeak "write" endpoint, over HTTPS (TLS) so the telemetry is encrypted
// in transit, matching the security claim in the report and presentation.
const char* server = "https://api.thingspeak.com/update";

X509List cert(TELEGRAM_CERTIFICATE_ROOT);          // root certificate used to trust Telegram over TLS
WiFiClientSecure secured_client;                    // the encrypted (TLS) connection
UniversalTelegramBot bot(BOT_TOKEN, secured_client); // the bot, authenticated by BOT_TOKEN

// ---- M2M serial pins to the Arduino ----------------------------------------
#define ARD_RX 13                  // GPIO13 (D7) receives from the Arduino
#define ARD_TX 12                  // GPIO12 (D6) sends to the Arduino
SoftwareSerial ardSerial(ARD_RX, ARD_TX);

// ---- Polling / timing intervals --------------------------------------------
unsigned long lastBotCheck = 0;
const unsigned long BOT_MTBS = 1000;        // check Telegram for new messages every 1 s
unsigned long lastThingSpeak = 0;
const unsigned long TS_INTERVAL = 16000;    // push telemetry every 16 s (within ThingSpeak's rate limit)

unsigned long lastArduinoPing = 0;          // time of the last message from the Arduino
bool isArduinoOffline = false;              // true if the Arduino has gone silent

// ---- Cached state (mirrors what the Arduino last reported) ------------------
int currentFill = 0;
float currentTemp = 0.0;
float currentHum = 0.0;
bool isFire = false;
bool isFull = false;
bool isOpen = false;
bool isMuted = false;

int currentVolumePercent = 50;
String lastVolumeUser = "System";

// ---- Password-gated reset state --------------------------------------------
int    pendingReset     = 0;   // 0 = none, 1 = ESP, 2 = Arduino
String pendingResetChat = "";  // chat_id that must supply the password

// ---- Multi-user session list (so alerts reach everyone using the bot) ------
const int MAX_USERS = 15;
String sessionUsers[MAX_USERS];
int userCount = 0;

// Build the Telegram reply keyboard (the row of buttons under the chat).
String getKeyboard() {
  String muteBtn = isMuted ? "Unmute (" + String(currentVolumePercent) + "%)" : "Mute (" + String(currentVolumePercent) + "%)";
  return "[[\"Status\", \"Open\"], [\"" + muteBtn + "\", \"Details\"], [\"Vol -\", \"Vol +\"], [\"Reset ESP\", \"Reset Arduino\"]]";
}

// Remember a chat_id the first time it talks to the bot (for broadcasting).
void addSessionUser(String chat_id) {
  for(int i = 0; i < userCount; i++) {
    if(sessionUsers[i] == chat_id) return;     // already known -> do nothing
  }
  if(userCount < MAX_USERS) {                  // room left -> add it
    sessionUsers[userCount] = chat_id;
    userCount++;
  }
}

// Send a plain message to every active user (used for routine action logs).
void broadcastAction(String message) {
  for(int i = 0; i < userCount; i++) {
    bot.sendMessage(sessionUsers[i], message, "");
  }
}

// Proactive alerts: send to every active session user, plus the configured
// owner (CHAT_ID) so the alert always lands even if nobody has messaged since boot.
void notifyAll(String message) {
  bool ownerSeen = false;
  for (int i = 0; i < userCount; i++) {
    bot.sendMessage(sessionUsers[i], message, "");
    if (sessionUsers[i] == CHAT_ID) ownerSeen = true;     // did we already include the owner?
  }
  if (!ownerSeen) bot.sendMessage(CHAT_ID, message, "");  // guarantee the owner always gets it
}

// Same as notifyAll, but also re-shows the button keyboard.
void notifyAllKb(String message) {
  bool ownerSeen = false;
  for (int i = 0; i < userCount; i++) {
    bot.sendMessageWithReplyKeyboard(sessionUsers[i], message, "", getKeyboard(), true);
    if (sessionUsers[i] == CHAT_ID) ownerSeen = true;
  }
  if (!ownerSeen) bot.sendMessageWithReplyKeyboard(CHAT_ID, message, "", getKeyboard(), true);
}

// ============================================================================
//  SETUP  (runs once at power-on)
// ============================================================================
void setup() {
  Serial.begin(9600);             // USB serial (for debugging on a PC)
  ardSerial.begin(9600);          // the M2M link to the Arduino

  WiFi.mode(WIFI_STA);            // station mode: join an existing Wi-Fi network
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  while (WiFi.status() != WL_CONNECTED) {   // wait until connected
    delay(500);
  }

  secured_client.setTrustAnchors(&cert);    // tell the TLS client which root cert to trust (Telegram)
  configTime(0, 0, "pool.ntp.org");         // get the real time from the internet (NTP)...
  time_t now = time(nullptr);
  while (now < 24 * 3600) {                 // ...TLS needs a valid clock to check certificates
    delay(100);
    now = time(nullptr);
  }

  // Drop any commands queued in Telegram while the bot was offline, so a
  // restart never replays old commands from the previous session. We advance
  // the update offset past the whole backlog WITHOUT calling handleNewMessages.
  int pendingUpdates = bot.getUpdates(bot.last_message_received + 1);
  while (pendingUpdates) {
    pendingUpdates = bot.getUpdates(bot.last_message_received + 1);
  }

  ArduinoOTA.setHostname("FrankenBin_Gateway");   // network name shown in the Arduino IDE
  ArduinoOTA.setPassword(OTA_PASSWORD);           // password required to push an OTA update
  ArduinoOTA.begin();

  bot.sendMessageWithReplyKeyboard(CHAT_ID, "FrankenBin is ONLINE", "", getKeyboard(), true);  // announce boot to the owner
  Serial.println("System booted and connected to Wi-Fi.");

  lastArduinoPing = millis();    // start the heartbeat timer
}

// ============================================================================
//  MAIN LOOP
// ============================================================================
void loop() {
  ArduinoOTA.handle();           // service any incoming OTA update request

  // ---- 1) Read and react to events coming FROM the Arduino -----------------
  if (ardSerial.available() > 0) {
    String msg = ardSerial.readStringUntil('\n');   // read one line
    msg.trim();

    lastArduinoPing = millis();  // any message = the Arduino is alive -> reset heartbeat

    if (isArduinoOffline) {      // it had been offline and just came back
      isArduinoOffline = false;
      notifyAllKb("Hardware connection restored! Arduino is back online.");
      Serial.println("Arduino is back online.");
    }

    Serial.println("Incoming from Arduino: " + msg);   // debug echo

    // ---- Decode the event by its prefix ----
    if (msg.startsWith("TEMP:")) {
      currentTemp = msg.substring(5).toFloat();        // cache temperature
    }
    else if (msg.startsWith("HUM:")) {
      currentHum = msg.substring(4).toFloat();         // cache humidity
    }
    else if (msg.startsWith("FILL:")) {
      currentFill = msg.substring(5).toInt();          // cache fill %
      if (currentFill < 80) {
        isFull = false;                                // dropped below 80 % -> no longer "full"
      }
    }
    else if (msg.startsWith("VOL:")) {
      String vol = msg.substring(4);
      currentVolumePercent = vol.toInt();
      isMuted = false;   // any volume change means the Arduino un-muted

      for(int i = 0; i < userCount; i++) {             // tell everyone who adjusted the volume
        bot.sendMessageWithReplyKeyboard(sessionUsers[i], "User " + lastVolumeUser + " adjusted the volume.", "", getKeyboard(), true);
      }
    }
    else if (msg.startsWith("LID:OPEN:")) {
      isOpen = true;
      char src = msg.charAt(9);                        // the source character after "LID:OPEN:"
      String reason = "Unknown";

      if (src == 'T') reason = "Telegram Bot";
      else if (src == 'H') reason = "Sensor (Hand movement)";
      else if (src == 'P') reason = "Foot pedal";
      notifyAll("Bin opened. Source: " + reason);      // log who opened it
    }
    else if (msg == "LID:CLOSED") {
      isOpen = false;
    }
    else if (msg == "ALARM:FIRE" && !isFire) {         // first fire message only (avoid spam)
      isFire = true;
      notifyAll("CRITICAL ALARM! FIRE!\nImmediate action required!");
    }
    else if (msg == "SYS:COOLDOWN") {
      isFire = false;
      notifyAll("Fire extinguished. System returned to normal.");
    }
    else if (msg == "ALARM:FULL" && !isFull) {
      isFull = true;
      notifyAll("Bin is full. Maintenance required.");
    }
    else if (msg == "SYS:BOOT") {
      // Arduino restarted: its lid is closed and state is fresh. Clear stale
      // flags so the gateway stops thinking the lid is still open.
      isOpen = false;
      isFire = false;
      isFull = false;
    }
  }

  // ---- 2) Heartbeat watch: warn if the Arduino has been silent > 20 s ------
  if (!isArduinoOffline && millis() - lastArduinoPing > 20000) {
    isArduinoOffline = true;
    notifyAllKb("WARNING: Hardware Offline! Arduino is not responding.");
    Serial.println("WARNING: Arduino heartbeat lost!");
  }

  // ---- 3) Push telemetry to ThingSpeak on a timer -------------------------
  if (millis() - lastThingSpeak > TS_INTERVAL) {
    sendToThingSpeak();
    lastThingSpeak = millis();
  }

  // ---- 4) Poll Telegram for new commands on a timer -----------------------
  if (millis() - lastBotCheck > BOT_MTBS) {
    int numNewMessages = bot.getUpdates(bot.last_message_received + 1);  // ask Telegram for messages
    while (numNewMessages) {                                            // process all of them...
      handleNewMessages(numNewMessages);
      numNewMessages = bot.getUpdates(bot.last_message_received + 1);    // ...until none are left
    }
    lastBotCheck = millis();
  }
}

// ============================================================================
//  HANDLE TELEGRAM MESSAGES  (commands coming FROM users)
// ============================================================================
void handleNewMessages(int numNewMessages) {
  for (int i = 0; i < numNewMessages; i++) {
    String chat_id = bot.messages[i].chat_id;       // who sent it
    String text = bot.messages[i].text;             // what they sent
    String senderName = bot.messages[i].from_name;  // their display name

    addSessionUser(chat_id);                        // remember them for broadcasts

    // Password gate: if a reset is pending for this chat, treat the next message
    // as the password attempt (same password as OTA, pulled from secrets.h).
    if (pendingReset != 0 && chat_id == pendingResetChat) {
      // Delete the password message so it never lingers in the chat history.
      bot.sendGetToTelegram("bot" + String(BOT_TOKEN) + "/deleteMessage?chat_id=" +
                            chat_id + "&message_id=" + String(bot.messages[i].message_id));
      if (text == OTA_PASSWORD) {                   // correct password
        if (pendingReset == 1) {                    // ...reset the ESP gateway
          bot.sendMessage(chat_id, "Password OK. ESP Gateway rebooting...", "");
          broadcastAction("User " + senderName + " reset the ESP Gateway.");
          pendingReset = 0; pendingResetChat = "";
          bot.getUpdates(bot.last_message_received + 1);   // flush before restart so it doesn't replay
          delay(1000);
          ESP.restart();
        } else {                                    // ...reset the Arduino
          if (!isArduinoOffline) {
            ardSerial.print('r');                   // send the reset command to the Arduino
            bot.sendMessage(chat_id, "Password OK. Arduino rebooting...", "");
            broadcastAction("User " + senderName + " reset the Arduino.");
          } else {
            bot.sendMessage(chat_id, "Cannot reset: Arduino is already offline.", "");
          }
          pendingReset = 0; pendingResetChat = "";
        }
      } else {                                       // wrong password -> cancel
        pendingReset = 0; pendingResetChat = "";
        bot.sendMessage(chat_id, "Wrong password. Reset cancelled.", "");
      }
      continue;                                      // done with this message
    }

    // ---- Normal buttons / commands ----
    if (text == "/start" || text == "/menu") {       // show the menu
      String menuHeader = isArduinoOffline ? "FrankenBin is OFFLINE" : "FrankenBin is ONLINE";
      bot.sendMessageWithReplyKeyboard(chat_id, menuHeader, "", getKeyboard(), true);
      broadcastAction("User " + senderName + " joined the bot session.");
    }
    else if (text.indexOf("Status") > -1) {          // report full system status
      long rssi = WiFi.RSSI();                        // Wi-Fi signal strength
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
    else if (text.indexOf("Open") > -1) {            // open the lid remotely
      if (isArduinoOffline) {
        bot.sendMessage(chat_id, "Cannot open: Hardware is offline.", "");
      } else if (isOpen) {
        bot.sendMessage(chat_id, "Lid is already open.", "");
      } else {
        if (isFire) ardSerial.print('c');            // during a fire -> 'c' (clear), else 'o' (open)
        else ardSerial.print('o');
        Serial.println("Command sent: Open");
        bot.sendMessage(chat_id, "Opening...", "");

        broadcastAction("User " + senderName + " opened the bin remotely.");
      }
    }
    else if (text.indexOf("Mute") > -1 || text.indexOf("Unmute") > -1) {   // toggle mute
      if (!isArduinoOffline) {
        isMuted = !isMuted;
        ardSerial.print('m');                        // tell the Arduino to toggle mute
        Serial.println("Command sent: Mute toggle");
        bot.sendMessageWithReplyKeyboard(chat_id, isMuted ? "System sounds muted" : "System sounds unmuted", "", getKeyboard(), true);

        broadcastAction("User " + senderName + (isMuted ? " muted" : " unmuted") + " the system sounds.");
      } else {
        bot.sendMessage(chat_id, "Cannot toggle mute: Hardware is offline.", "");
      }
    }
    else if (text.indexOf("Vol -") > -1) {           // volume down
      if (!isArduinoOffline) {
        lastVolumeUser = senderName;
        ardSerial.print('-');
        Serial.println("Command sent: Vol -");
      } else {
        bot.sendMessage(chat_id, "Cannot change volume: Hardware is offline.", "");
      }
    }
    else if (text.indexOf("Vol +") > -1) {           // volume up
      if (!isArduinoOffline) {
        lastVolumeUser = senderName;
        ardSerial.print('+');
        Serial.println("Command sent: Vol +");
      } else {
        bot.sendMessage(chat_id, "Cannot change volume: Hardware is offline.", "");
      }
    }
    else if (text.indexOf("Reset ESP") > -1) {       // ask for password, then reset the gateway
      pendingReset = 1; pendingResetChat = chat_id;
      bot.sendMessage(chat_id, "Enter the password to confirm ESP Gateway reset:", "");
    }
    else if (text.indexOf("Reset Arduino") > -1) {   // ask for password, then reset the Arduino
      if (!isArduinoOffline) {
        pendingReset = 2; pendingResetChat = chat_id;
        bot.sendMessage(chat_id, "Enter the password to confirm Arduino reset:", "");
      } else {
        bot.sendMessage(chat_id, "Cannot reset: Arduino is already offline.", "");
      }
    }
    else if (text.indexOf("Details") > -1) {         // show the inline link buttons
      String inlineKeyboard = "[[{\"text\":\"About Creator\", \"url\":\"https://www.linkedin.com/in/marin-vacarciuc-b74b4b206\"}], "
                              "[{\"text\":\"ThingSpeak Charts\", \"url\":\"https://thingspeak.mathworks.com/channels/3405117\"}], "
                              "[{\"text\":\"Source Code\", \"url\":\"https://github.com/MarinVacarciuc/FrankenBin\"}]]";
      bot.sendMessageWithInlineKeyboard(chat_id, "Additional info:", "", inlineKeyboard);

      broadcastAction("User " + senderName + " viewed project details.");
    }
    else if (text == "/laws") {                      // hidden easter-egg: "The Three Laws of FrankenBin"
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
    else if (text == "/firetest") {                  // hidden command: trigger the real fire path for a demo
      if (!isArduinoOffline) {
        ardSerial.print('F');                        // 'F' -> Arduino runs triggerFireAlarm()
        bot.sendMessage(chat_id, "Fire-alarm self-test triggered. The lid will shut and the siren will sound. Press Open (or the pedal) to clear.", "");
        broadcastAction(senderName + " ran the fire-alarm self-test.");
      } else {
        bot.sendMessage(chat_id, "Cannot run fire test: hardware is offline.", "");
      }
    }
  }
}

// ============================================================================
//  THINGSPEAK UPLOAD  (a REST API call: HTTP GET with the values as parameters)
//  Endpoint: /update ; api_key authenticates ; field1..field4 carry the data.
// ============================================================================
void sendToThingSpeak() {
  if (WiFi.status() == WL_CONNECTED && !isArduinoOffline) {   // only if online and the Arduino is alive
    WiFiClientSecure tsClient;      // separate TLS client for ThingSpeak (the bot keeps its own)
    tsClient.setInsecure();         // encrypt the channel over TLS; skip cert pinning (telemetry is non-personal)
    HTTPClient http;
    long rssi = WiFi.RSSI();

    // Build the request URL with the four telemetry fields.
    String url = String(server) + "?api_key=" + String(TS_API_KEY) +
                 "&field1=" + String(currentFill) +     // fill %
                 "&field2=" + String(currentTemp) +     // temperature
                 "&field3=" + String(currentHum) +      // humidity
                 "&field4=" + String(rssi);             // Wi-Fi signal
    http.begin(tsClient, url);       // HTTPS request over TLS
    http.GET();                     // send it
    http.end();                     // close the connection
    Serial.println("Telemetry and RSSI sent to ThingSpeak (HTTPS).");
  }
}
