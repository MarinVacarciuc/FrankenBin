// FrankenBin v3.7 — production firmware.
// Arduino Uno: hardware FSM. NodeMCU ESP8266: networking (Telegram / ThingSpeak / OTA).
// M2M: HW Serial pins 0/1 ↔ NodeMCU SoftwareSerial D6/D7, 9600 baud, cross-wired.
//
// FSM states:
//   IDLE → OPENING → OPEN → CLOSING → IDLE
//                               └──────→ FULL_LOCKED  (bin ≥ 90 % full)
//   any state ──────────────────────────→ FIRE_ALARM  (temp ≥ FIRE_TEMP x2 readings)
//
// Serial protocol sent TO NodeMCU:
//   SYS:BOOT / SYS:CRASH        — startup events
//   LID:OPEN / LID:CLOSED       — lid transitions
//   FILL:<0-100>                 — fill % (once per close cycle)
//   TEMP:<float> / HUM:<float>  — climate (every 5 s)
//   SOUND:<vol%>:<0|1>          — volume + mute (single atomic message)
//   ALARM:FULL / ALARM:FIRE     — alerts
//
// Commands received FROM NodeMCU (single chars):
//   'o' open  'm' mute  '+'/'-' volume  'r' reset (two 'r' within 1 s required)
//
// !! Disconnect pins 0/1 before uploading via USB.

#include <SoftwareSerial.h>
#include <DFRobotDFPlayerMini.h>
#include <DHT.h>
#include <EEPROM.h>
#include <avr/wdt.h>

// =============================================================================
// Pin assignments
// MP3_RX/TX named from Arduino's perspective: Arduino RX ← DFPlayer TX, etc.
// =============================================================================
const int MP3_RX     = 11;
const int MP3_TX     = 10;
const int BUSY_PIN   =  4;   // DFPlayer BUSY: LOW while playing
const int DHT_PIN    =  7;
const int AIN1       =  8;
const int AIN2       =  9;
const int PWMA       =  5;
const int BUTTON_PIN = 12;
const int TRIG_1 = A0; const int ECHO_1 = A1;   // front sensor (hand detect)
const int TRIG_2 = A2; const int ECHO_2 = A3;   // lid sensor (fill / anti-pinch)

// =============================================================================
// Thresholds
// =============================================================================
const int   OPEN_DIST       = 20;    // cm — hand triggers open
const int   HOLD_DIST       = 30;    // cm — object in zone: keep lid open
const int   ANTIPINCH_CM    =  4;    // cm — reverse if detected while closing
const int   FULL_DIST       =  6;    // cm — sensor-to-trash when bin is full
const int   EMPTY_DIST      = 40;    // cm — sensor-to-bottom when bin is empty
const int   CLOSE_DELAY_MS  = 1000;  // ms — pause after zone clears before closing
const float FIRE_TEMP       = 35.0;  // °C — fire threshold (demo value)
const int   FIRE_CONFIRM    =  2;    // consecutive hot readings to trigger alarm

// Actuator travel times (ms)
const unsigned long ACT_EXTEND_MS  = 2500;
const unsigned long ACT_RETRACT_MS = 1900;

// =============================================================================
// EEPROM crash flag
// =============================================================================
const uint8_t CRASH_FLAG_ADDR = 0;
const uint8_t CRASH_MAGIC     = 0xAB;

// =============================================================================
// FSM
// =============================================================================
enum State { IDLE, OPENING, OPEN, CLOSING, FULL_LOCKED, FIRE_ALARM };
State currentState = IDLE;

// =============================================================================
// Peripherals
// =============================================================================
SoftwareSerial       mp3Serial(MP3_RX, MP3_TX);
DFRobotDFPlayerMini  myMP3;
DHT                  dht(DHT_PIN, DHT11);

// =============================================================================
// Non-blocking actuator driver
//
// CLOSING still manages the motor directly in its own polling loop (anti-pinch
// requires tight sensor sampling, so it stays blocking).
// OPENING and FIRE_ALARM use this driver so loop() keeps running during travel.
// =============================================================================
enum ActPhase { ACT_IDLE, ACT_EXTENDING, ACT_RETRACTING };
ActPhase     actPhase  = ACT_IDLE;
unsigned long actStartMs = 0;

void startExtend()  { actuatorExtend();  actPhase = ACT_EXTENDING;  actStartMs = millis(); }
void startRetract() { actuatorRetract(); actPhase = ACT_RETRACTING; actStartMs = millis(); }
bool actDone()      { return actPhase == ACT_IDLE; }

// Call once per loop() to advance actuator timing without delay().
void actuatorTick() {
  if (actPhase == ACT_EXTENDING  && millis() - actStartMs >= ACT_EXTEND_MS)  { actuatorStop(); actPhase = ACT_IDLE; }
  if (actPhase == ACT_RETRACTING && millis() - actStartMs >= ACT_RETRACT_MS) { actuatorStop(); actPhase = ACT_IDLE; }
}

// =============================================================================
// Runtime state
// =============================================================================
bool          isMuted         = false;
int           currentVolume   = 15;     // DFPlayer scale 0-30
int           welcomeTrack    = 2;      // cycles 2-4
int           angryTrack      = 6;      // cycles 6-8
bool          timerStarted    = false;  // OPEN state: zone-clear countdown active
unsigned long zoneClearTime   = 0;
unsigned long lastSensorMs    = 0;
unsigned long lastFireAlertMs = 0;
int           fireReadings    = 0;      // consecutive above-threshold DHT readings

// OPENING sub-state flags
bool openingActStarted  = false;  // has startExtend() been called this opening?
bool openingAudioDone   = false;  // have we queued the welcome track + 250 ms wait?

// FIRE_ALARM sub-state flag
bool fireAlarmLidOpen = false;    // has the lid been confirmed fully open?

// Helpers to transition into OPENING cleanly from anywhere
void beginOpening() {
  currentState       = OPENING;
  openingActStarted  = false;
  openingAudioDone   = false;
}

// =============================================================================
// Low-level drivers
// =============================================================================
void actuatorExtend()  { digitalWrite(AIN1, HIGH); digitalWrite(AIN2, LOW);  analogWrite(PWMA, 255); }
void actuatorRetract() { digitalWrite(AIN1, LOW);  digitalWrite(AIN2, HIGH); analogWrite(PWMA, 255); }
void actuatorStop()    { digitalWrite(AIN1, LOW);  digitalWrite(AIN2, LOW);  analogWrite(PWMA,   0); }

long getDistance(int trig, int echo) {
  digitalWrite(trig, LOW);  delayMicroseconds(2);
  digitalWrite(trig, HIGH); delayMicroseconds(10);
  digitalWrite(trig, LOW);
  long d = pulseIn(echo, HIGH, 30000);
  return (d > 0) ? d / 58 : 999;
}

// Takes 3 samples, sends FILL: to NodeMCU, returns 0-100 (-1 on sensor error).
int readFillPercent() {
  long sum = 0; int n = 0;
  for (int i = 0; i < 3; i++) {
    long d = getDistance(TRIG_2, ECHO_2);
    if (d > 0 && d < EMPTY_DIST + 10) { sum += d; n++; }
    delay(50);
  }
  if (n == 0) return -1;
  int pct = constrain(map(sum / n, EMPTY_DIST, FULL_DIST, 0, 100), 0, 100);
  Serial.print(F("FILL:")); Serial.println(pct);
  return pct;
}

// Debounced button read; watchdog-safe (3 s max hold).
bool checkButton() {
  if (digitalRead(BUTTON_PIN) != LOW) return false;
  delay(50);
  if (digitalRead(BUTTON_PIN) != LOW) return false;
  unsigned long t = millis();
  while (digitalRead(BUTTON_PIN) == LOW) {
    if (millis() - t > 3000) break;
    wdt_reset();
    delay(10);
  }
  return true;
}

void reportVolumeState() {
  int pct = map(currentVolume, 0, 30, 0, 100);
  Serial.print(F("SOUND:")); Serial.print(pct); Serial.print(':'); Serial.println(isMuted ? 1 : 0);
}

// =============================================================================
// Command handler (called for every byte received from NodeMCU)
// =============================================================================
void processCommand(char cmd) {
  switch (cmd) {
    case '+':
      currentVolume = constrain(currentVolume + 2, 0, 30);
      if (!isMuted) myMP3.volume(currentVolume);
      reportVolumeState();
      break;
    case '-':
      currentVolume = constrain(currentVolume - 2, 0, 30);
      if (!isMuted) myMP3.volume(currentVolume);
      reportVolumeState();
      break;
    case 'm':
      isMuted = !isMuted;
      myMP3.volume(isMuted ? 0 : currentVolume);
      reportVolumeState();
      break;
    case 'o':
      if (currentState == IDLE || currentState == FULL_LOCKED) beginOpening();
      break;
    case 'r': {
      // Two 'r' bytes within 1 s — guards against SoftwareSerial noise
      // (NodeMCU occasionally emits 0x72 during TLS/HTTPS operations).
      static bool          pending     = false;
      static unsigned long pendingTime = 0;
      if (!pending || millis() - pendingTime >= 1000) {
        pending     = true;
        pendingTime = millis();
      } else {
        void (*reset)(void) = 0; reset();
      }
      break;
    }
  }
}

// =============================================================================
// Watchdog: interrupt + reset mode
// wdt_enable() alone only sets WDE (reset only). We need WDIE set too so the
// ISR fires before reset, giving us time to write the crash flag to EEPROM.
// =============================================================================
ISR(WDT_vect) {
  EEPROM.write(CRASH_FLAG_ADDR, CRASH_MAGIC);
  // WDE remains set → CPU resets immediately after ISR returns.
}

void enableWatchdog() {
  cli();
  MCUSR = 0;
  WDTCSR |= (1 << WDCE) | (1 << WDE);
  WDTCSR  = (1 << WDIE) | (1 << WDE) | (1 << WDP3);   // interrupt+reset, 4 s
  sei();
}

// =============================================================================
// Setup
// =============================================================================
void setup() {
  wdt_disable();
  Serial.begin(9600);
  mp3Serial.begin(9600);
  dht.begin();

  pinMode(TRIG_1, OUTPUT); pinMode(ECHO_1, INPUT);
  pinMode(TRIG_2, OUTPUT); pinMode(ECHO_2, INPUT);
  pinMode(AIN1, OUTPUT); pinMode(AIN2, OUTPUT); pinMode(PWMA, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(BUSY_PIN, INPUT);

  delay(1000);

  if (EEPROM.read(CRASH_FLAG_ADDR) == CRASH_MAGIC) {
    EEPROM.write(CRASH_FLAG_ADDR, 0x00);
    Serial.println(F("SYS:CRASH"));
  }

  if (myMP3.begin(mp3Serial)) {
    myMP3.volume(currentVolume);
    delay(500);
    myMP3.playMp3Folder(1);
    delay(1500);
  }

  // Active anti-pinch on startup close (blocking is fine here — runs once).
  actuatorRetract();
  unsigned long t0 = millis();
  while (millis() - t0 < ACT_RETRACT_MS) {
    wdt_reset();
    if (getDistance(TRIG_2, ECHO_2) < ANTIPINCH_CM) {
      actuatorStop(); delay(200); actuatorExtend(); delay(500); actuatorStop();
      break;
    }
    delay(50);
  }
  actuatorStop();

  currentState = IDLE;
  Serial.println(F("SYS:BOOT"));
  enableWatchdog();
}

// =============================================================================
// Main loop
// =============================================================================
void loop() {
  wdt_reset();

  // Advance non-blocking actuator (OPENING / FIRE_ALARM states use this).
  actuatorTick();

  // Drain all pending serial bytes from NodeMCU.
  while (Serial.available() > 0) processCommand(Serial.read());

  // Physical button — open from IDLE or FULL_LOCKED.
  // (During CLOSING the button is missed because CLOSING has its own polling
  // loop; 1.9 s window is acceptable for this project.)
  if (checkButton()) {
    if (currentState == IDLE || currentState == FULL_LOCKED) {
      myMP3.stop();
      beginOpening();
    }
  }

  // ---- Periodic DHT11 read (every 5 s) ------------------------------------
  if (millis() - lastSensorMs >= 5000) {
    wdt_reset();
    float t = dht.readTemperature();
    float h = dht.readHumidity();

    if (!isnan(t)) {
      Serial.print(F("TEMP:")); Serial.println(t, 1);

      if (t >= FIRE_TEMP) {
        fireReadings++;
        if (fireReadings >= FIRE_CONFIRM && currentState != FIRE_ALARM) {
          // Trigger fire alarm — non-blocking: lid opens via actuatorTick().
          currentState      = FIRE_ALARM;
          fireAlarmLidOpen  = false;
          lastFireAlertMs   = millis();
          Serial.println(F("ALARM:FIRE"));
          startExtend();
        }
      } else {
        fireReadings = 0;
        // Auto-exit: temperature is safe again — close the lid.
        if (currentState == FIRE_ALARM && fireAlarmLidOpen && actDone()) {
          startRetract();
          // FIRE_ALARM case below detects ACT_RETRACTING completion.
        }
      }
    }

    if (!isnan(h)) { Serial.print(F("HUM:")); Serial.println(h, 1); }
    lastSensorMs = millis();
  }

  // ---- FSM -----------------------------------------------------------------
  //
  // The Three Laws of FrankenBin  (Asimov, 1942 — adapted for rubbish, 2026)
  //
  // LAW I   "A bin shall not harm a human being."
  //          → CLOSING polls TRIG_2/ECHO_2 every 50 ms; reverses if < ANTIPINCH_CM.
  //          → FULL_LOCKED ignores the 'o' command: Law I overrides Law II.
  //
  // LAW II  "A bin shall obey orders given by human beings,
  //          except where such orders conflict with Law I."
  //          → processCommand() executes 'o','m','+','-','r' from NodeMCU.
  //
  // LAW III "A bin shall protect its own existence,
  //          as long as this does not conflict with Laws I or II."
  //          → AVR watchdog (4 s timeout) resets a hung firmware automatically.
  //          → ISR(WDT_vect) writes CRASH_MAGIC to EEPROM before reset;
  //            SYS:CRASH is sent to Telegram on the next boot.
  //
  switch (currentState) {

    // ------------------------------------------------------------------
    case IDLE: {
      long d1 = getDistance(TRIG_1, ECHO_1);
      if (d1 > 0 && d1 < OPEN_DIST) beginOpening();
      delay(150);
      break;
    }

    // ------------------------------------------------------------------
    // Non-blocking open sequence:
    //   Phase 1 — startExtend() → actuatorTick() drives it for 2500 ms
    //   Phase 2 — queue welcome track + 250 ms DFPlayer BUSY settle
    //   Phase 3 — enter OPEN state
    case OPENING: {
      if (!openingActStarted) {
        Serial.println(F("LID:OPEN"));
        startExtend();
        openingActStarted = true;
      } else if (actDone() && !openingAudioDone) {
        if (!isMuted) {
          myMP3.playMp3Folder(welcomeTrack);
          welcomeTrack = (welcomeTrack >= 4) ? 2 : welcomeTrack + 1;
          // DFPlayer needs ~250 ms to pull BUSY LOW after a play command.
          // If we skip this, OPEN state sees BUSY=HIGH and starts closing immediately.
          delay(250);
        }
        openingAudioDone = true;
      } else if (actDone() && openingAudioDone) {
        currentState = OPEN;
        timerStarted = false;
      }
      break;
    }

    // ------------------------------------------------------------------
    case OPEN: {
      long d2 = getDistance(TRIG_2, ECHO_2);
      if (d2 > 0 && d2 < HOLD_DIST) {
        timerStarted = false;
      } else {
        if (!timerStarted) { zoneClearTime = millis(); timerStarted = true; }
        if (millis() - zoneClearTime >= CLOSE_DELAY_MS && digitalRead(BUSY_PIN) == HIGH)
          currentState = CLOSING;
      }
      delay(150);
      break;
    }

    // ------------------------------------------------------------------
    // CLOSING stays blocking: anti-pinch needs tight polling.
    case CLOSING: {
      bool pinched = false;
      actuatorRetract();
      unsigned long closeStart = millis();
      while (millis() - closeStart < ACT_RETRACT_MS) {
        wdt_reset();
        if (getDistance(TRIG_2, ECHO_2) < ANTIPINCH_CM) {
          actuatorStop(); delay(100); actuatorExtend(); delay(400); actuatorStop();
          Serial.println(F("LID:OPEN"));
          currentState = OPEN;
          timerStarted = false;
          pinched      = true;
          break;
        }
        delay(50);
      }
      if (!pinched) {
        actuatorStop();
        Serial.println(F("LID:CLOSED"));
        int pct = readFillPercent();
        if (pct >= 90) {
          currentState = FULL_LOCKED;
          Serial.println(F("ALARM:FULL"));
        } else {
          currentState = IDLE;
        }
      }
      break;
    }

    // ------------------------------------------------------------------
    case FULL_LOCKED: {
      long d1 = getDistance(TRIG_1, ECHO_1);
      if (d1 > 0 && d1 < OPEN_DIST && !isMuted) {
        myMP3.playMp3Folder(angryTrack);
        angryTrack = (angryTrack >= 8) ? 6 : angryTrack + 1;
        delay(500);
      }
      delay(150);
      break;
    }

    // ------------------------------------------------------------------
    // FIRE_ALARM phases (all driven by actuatorTick(), no blocking delays):
    //   1. !fireAlarmLidOpen && !actDone()  → lid still extending
    //   2. !fireAlarmLidOpen &&  actDone()  → lid just reached full open
    //   3.  fireAlarmLidOpen && retracting  → temperature dropped, auto-closing
    //   4.  fireAlarmLidOpen &&  actDone()  → alarm active, lid held open
    case FIRE_ALARM: {
      if (!fireAlarmLidOpen) {
        if (actDone()) {
          // Lid reached full open position.
          Serial.println(F("LID:OPEN"));
          fireAlarmLidOpen = true;
        }
        // else: still extending — actuatorTick() handles it, nothing to do here.

      } else if (actPhase == ACT_RETRACTING) {
        // Temperature safe — closing after alarm.
        if (actDone()) {
          Serial.println(F("LID:CLOSED"));
          currentState = IDLE;
        }

      } else {
        // Alarm active, lid held open. Repeat alert every 10 s.
        if (millis() - lastFireAlertMs > 10000) {
          Serial.println(F("ALARM:FIRE"));
          lastFireAlertMs = millis();
        }
        delay(500);
      }
      break;
    }
  }
}
