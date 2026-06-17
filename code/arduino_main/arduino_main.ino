// FrankenBin v3.6 — production firmware.
// Arduino Uno: hardware FSM. NodeMCU ESP8266: networking (Telegram / ThingSpeak / OTA).
// M2M: HW Serial pins 0/1 ↔ NodeMCU SoftwareSerial D6/D7, 9600 baud, cross-wired.
//
// FSM states:
//   IDLE → OPENING → OPEN → CLOSING → IDLE
//                               └──────→ FULL_LOCKED  (bin ≥ 90 % full)
//   any state ──────────────────────────→ FIRE_ALARM  (temp ≥ FIRE_TEMP for 2 readings)
//
// Serial protocol sent TO NodeMCU:
//   SYS:BOOT / SYS:CRASH   — startup events
//   LID:OPEN / LID:CLOSED  — lid transitions
//   FILL:<0-100>            — fill % (sent once per close)
//   TEMP:<float>            — temperature °C  (every 5 s)
//   HUM:<float>             — humidity %      (every 5 s)
//   SOUND:<vol%>:<0|1>      — volume + mute state (single atomic update)
//   ALARM:FULL              — bin full
//   ALARM:FIRE              — fire detected (repeated every 10 s)
//
// Commands received FROM NodeMCU (single chars):
//   'o' open   '+'/'-' volume   'm' mute   'r' reset (requires two 'r' within 1 s)
//
// !! Disconnect pins 0/1 before uploading via USB.

#include <SoftwareSerial.h>
#include <DFRobotDFPlayerMini.h>
#include <DHT.h>
#include <EEPROM.h>
#include <avr/wdt.h>

// --- Pin assignments ---------------------------------------------------------
//   MP3_RX/TX are from Arduino's perspective: Arduino RX ← DFPlayer TX, etc.
const int MP3_RX     = 11;   // Arduino RX ← DFPlayer TX
const int MP3_TX     = 10;   // Arduino TX → DFPlayer RX
const int BUSY_PIN   =  4;   // DFPlayer BUSY (LOW while playing)
const int DHT_PIN    =  7;
const int AIN1       =  8;
const int AIN2       =  9;
const int PWMA       =  5;
const int BUTTON_PIN = 12;
const int TRIG_1 = A0; const int ECHO_1 = A1;  // front sensor  (hand detect)
const int TRIG_2 = A2; const int ECHO_2 = A3;  // lid sensor    (fill / anti-pinch)

// --- Thresholds --------------------------------------------------------------
const int   OPEN_DIST    = 20;   // cm — hand detection distance
const int   HOLD_DIST    = 30;   // cm — object-in-zone: keep lid open
const int   ANTIPINCH_CM =  4;   // cm — anti-pinch trigger during closing
const int   FULL_DIST    =  6;   // cm — "bin full" distance (sensor to trash)
const int   EMPTY_DIST   = 40;   // cm — "bin empty" distance
const int   CLOSE_DELAY  = 1000; // ms — idle time before starting to close
const float FIRE_TEMP    = 35.0; // °C — fire alarm threshold (demo value)
const int   FIRE_CONFIRM =  2;   // consecutive readings required to trigger fire alarm

// --- EEPROM crash flag -------------------------------------------------------
const uint8_t CRASH_FLAG_ADDR = 0;
const uint8_t CRASH_MAGIC     = 0xAB;

// --- State machine -----------------------------------------------------------
enum State { IDLE, OPENING, OPEN, CLOSING, FULL_LOCKED, FIRE_ALARM };
State currentState = IDLE;

// --- Peripherals -------------------------------------------------------------
SoftwareSerial mp3Serial(MP3_RX, MP3_TX);
DFRobotDFPlayerMini myMP3;
DHT dht(DHT_PIN, DHT11);

// --- Runtime state -----------------------------------------------------------
bool          isMuted       = false;
int           currentVolume = 15;       // DFPlayer scale 0-30
int           welcomeTrack  = 2;        // cycles 2-4
int           angryTrack    = 6;        // cycles 6-8
bool          timerStarted  = false;
unsigned long zoneClearTime = 0;
unsigned long lastSensorMs  = 0;
unsigned long lastFireAlertMs = 0;
int           fireReadings  = 0;        // consecutive above-threshold readings

// --- Actuator helpers --------------------------------------------------------
void actuatorExtend()  { digitalWrite(AIN1, HIGH); digitalWrite(AIN2, LOW);  analogWrite(PWMA, 255); }
void actuatorRetract() { digitalWrite(AIN1, LOW);  digitalWrite(AIN2, HIGH); analogWrite(PWMA, 255); }
void actuatorStop()    { digitalWrite(AIN1, LOW);  digitalWrite(AIN2, LOW);  analogWrite(PWMA,   0); }

// --- Sensor helpers ----------------------------------------------------------
long getDistance(int trig, int echo) {
  digitalWrite(trig, LOW);  delayMicroseconds(2);
  digitalWrite(trig, HIGH); delayMicroseconds(10);
  digitalWrite(trig, LOW);
  long d = pulseIn(echo, HIGH, 30000);
  return (d > 0) ? d / 58 : 999;
}

// Returns fill 0-100 and sends FILL: to NodeMCU. Returns -1 on sensor error.
int readFillPercent() {
  long sum = 0; int n = 0;
  for (int i = 0; i < 3; i++) {
    long d = getDistance(TRIG_2, ECHO_2);
    if (d > 0 && d < EMPTY_DIST + 10) { sum += d; n++; }
    delay(50);
  }
  if (n == 0) return -1;
  long avg = sum / n;
  int pct = constrain(map(avg, EMPTY_DIST, FULL_DIST, 0, 100), 0, 100);
  Serial.print(F("FILL:")); Serial.println(pct);
  return pct;
}

// Button read with debounce and watchdog-safe hold timeout.
bool checkButton() {
  if (digitalRead(BUTTON_PIN) != LOW) return false;
  delay(50);
  if (digitalRead(BUTTON_PIN) != LOW) return false;
  unsigned long t = millis();
  while (digitalRead(BUTTON_PIN) == LOW) {
    if (millis() - t > 3000) break;  // 3 s max hold (well under 4 s watchdog)
    wdt_reset();
    delay(10);
  }
  return true;
}

// --- Volume / mute -----------------------------------------------------------
void reportVolumeState() {
  int pct = map(currentVolume, 0, 30, 0, 100);
  Serial.print(F("SOUND:")); Serial.print(pct); Serial.print(':'); Serial.println(isMuted ? 1 : 0);
}

// --- Command handler ---------------------------------------------------------
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
      if (currentState == IDLE || currentState == FULL_LOCKED) currentState = OPENING;
      break;
    case 'r': {
      // Two 'r' bytes required within 1 s — guards against SoftwareSerial noise
      // (NodeMCU SoftwareSerial occasionally emits 0x72 during TLS operations).
      static bool          pendingReset = false;
      static unsigned long pendingTime  = 0;
      if (!pendingReset || millis() - pendingTime >= 1000) {
        pendingReset = true;
        pendingTime  = millis();
      } else {
        void (*reset)(void) = 0; reset();
      }
      break;
    }
  }
}

// --- Watchdog ISR ------------------------------------------------------------
// wdt_enable() alone only sets WDE (reset). For the ISR to fire we need WDIE too.
// enableWatchdog() below sets both bits manually (interrupt + reset mode):
//   first timeout → ISR fires (writes crash flag) → system resets immediately after.
ISR(WDT_vect) {
  EEPROM.write(CRASH_FLAG_ADDR, CRASH_MAGIC);
  // WDE is still set; CPU resets as ISR returns.
}

void enableWatchdog() {
  cli();
  MCUSR = 0;                               // clear any prior reset flags
  WDTCSR |= (1 << WDCE) | (1 << WDE);     // unlock WDTCSR for one write
  WDTCSR  = (1 << WDIE) | (1 << WDE)      // interrupt + reset mode
           | (1 << WDP3);                  // timeout = 4 s
  sei();
}

// --- Setup -------------------------------------------------------------------
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
    myMP3.playMp3Folder(1);   // boot sound (track 1)
    delay(1500);
  }

  // Active anti-pinch on startup close
  actuatorRetract();
  unsigned long t0 = millis();
  while (millis() - t0 < 1900) {
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

// --- Loop --------------------------------------------------------------------
void loop() {
  wdt_reset();

  // Consume all available serial bytes (not just one per loop iteration)
  while (Serial.available() > 0) processCommand(Serial.read());

  // Button: open from IDLE, FULL_LOCKED, or interrupt an ongoing CLOSING
  if (checkButton()) {
    if (currentState == IDLE || currentState == FULL_LOCKED || currentState == CLOSING) {
      myMP3.stop();
      currentState = OPENING;
    }
  }

  // Periodic sensor read (every 5 s)
  if (millis() - lastSensorMs >= 5000) {
    wdt_reset();
    float t = dht.readTemperature();
    float h = dht.readHumidity();

    if (!isnan(t)) {
      Serial.print(F("TEMP:")); Serial.println(t, 1);

      if (t >= FIRE_TEMP) {
        fireReadings++;
        if (fireReadings >= FIRE_CONFIRM && currentState != FIRE_ALARM) {
          currentState    = FIRE_ALARM;
          lastFireAlertMs = millis();   // prevents immediate repeat in FIRE_ALARM case
          Serial.println(F("ALARM:FIRE"));
          actuatorExtend(); delay(2500); actuatorStop();
          Serial.println(F("LID:OPEN"));
        }
      } else {
        fireReadings = 0;
        // Auto-exit FIRE_ALARM when temperature is safe again
        if (currentState == FIRE_ALARM) {
          Serial.println(F("LID:CLOSED"));
          actuatorRetract(); delay(1900); actuatorStop();
          currentState = IDLE;
        }
      }
    }

    if (!isnan(h)) { Serial.print(F("HUM:")); Serial.println(h, 1); }
    lastSensorMs = millis();
  }

  // FSM
  switch (currentState) {

    case IDLE: {
      long d1 = getDistance(TRIG_1, ECHO_1);
      if (d1 > 0 && d1 < OPEN_DIST) currentState = OPENING;
      delay(150);
      break;
    }

    case OPENING: {
      Serial.println(F("LID:OPEN"));
      actuatorExtend(); delay(2500); actuatorStop();
      if (!isMuted) {
        myMP3.playMp3Folder(welcomeTrack);
        welcomeTrack = (welcomeTrack >= 4) ? 2 : welcomeTrack + 1;
        // DFPlayer needs ~250 ms to assert BUSY after a play command.
        // Without this, OPEN state sees BUSY=HIGH and starts the close timer immediately.
        delay(250);
      }
      currentState  = OPEN;
      timerStarted  = false;
      break;
    }

    case OPEN: {
      long d2 = getDistance(TRIG_2, ECHO_2);
      if (d2 > 0 && d2 < HOLD_DIST) {
        timerStarted = false;
      } else {
        if (!timerStarted) { zoneClearTime = millis(); timerStarted = true; }
        if (millis() - zoneClearTime >= CLOSE_DELAY && digitalRead(BUSY_PIN) == HIGH)
          currentState = CLOSING;
      }
      delay(150);
      break;
    }

    case CLOSING: {
      bool pinched = false;
      actuatorRetract();
      unsigned long closeStart = millis();
      while (millis() - closeStart < 1900) {
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

    case FIRE_ALARM: {
      if (millis() - lastFireAlertMs > 10000) {
        Serial.println(F("ALARM:FIRE"));
        lastFireAlertMs = millis();
      }
      delay(500);
      break;
    }
  }
}
