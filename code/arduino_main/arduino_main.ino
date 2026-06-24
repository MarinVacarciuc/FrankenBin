// ============================================================================
//  SMARTBIN (FrankenBin) — ARDUINO UNO firmware  (v3.3 base, Production Mode)
// ----------------------------------------------------------------------------
//  ROLE OF THIS BOARD: the "brain". The Arduino Uno runs all the real-time,
//  safety-critical logic as a 6-state finite-state machine (FSM):
//      IDLE -> OPENING -> OPEN -> CLOSING -> FULL_LOCKED / FIRE_ALARM
//  It reads the sensors, drives the lid motor, plays audio, and decides
//  everything that must happen FAST and must keep working even with no Wi-Fi.
//
//  IT DOES NOT TOUCH THE INTERNET. It only sends short text "events" over a
//  serial cable (pins 0/1) to the ESP8266 gateway, e.g. "ALARM:FIRE", "FILL:73".
//  The ESP8266 is the only board that talks to Wi-Fi / ThingSpeak / Telegram.
//
//  RELIABILITY: a hardware Watchdog Timer auto-restarts the board if the code
//  ever freezes (see ISR(WDT_vect) and the setup() watchdog block).
//
//  M2M LINK: Hardware Serial pins 0 (RX) / 1 (TX), cross-wired to the ESP8266,
//  9600 baud.  IMPORTANT: disconnect pins 0/1 before uploading firmware by USB
//  (the USB and pin-0/1 share the same serial line).
// ============================================================================

// ---- Libraries -------------------------------------------------------------
#include <SoftwareSerial.h>          // extra serial port (for the MP3 player)
#include <DFRobotDFPlayerMini.h>     // driver for the DFPlayer Mini audio module
#include <DHT.h>                      // driver for the DHT11 temperature sensor
#include <avr/wdt.h>                  // AVR hardware Watchdog Timer registers
#include <EEPROM.h>                   // tiny non-volatile memory (survives reset)

// ---- The six states the bin can be in (the FSM) ----------------------------
enum State { IDLE, OPENING, OPEN, CLOSING, FULL_LOCKED, FIRE_ALARM };
State currentState = IDLE;            // we start in IDLE (waiting for a user)

// ---- Pin map (which component is wired to which Arduino pin) ----------------
const int TRIG_1 = A0; const int ECHO_1 = A1;   // ultrasonic #1: FRONT (detects an approaching user)
const int TRIG_2 = A2; const int ECHO_2 = A3;   // ultrasonic #2: INNER (fill level + anti-pinch)
const int AIN1 = 8; const int AIN2 = 9; const int PWMA = 5;  // TB6612FNG motor driver -> lid actuator (AIN1/AIN2 = direction, PWMA = speed)
const int BUTTON_PIN = 12; const int BUSY_PIN = 4; const int DHT_PIN = 7;  // push button; DFPlayer BUSY (LOW = playing); DHT11 data
const int DF_RX = 10; const int DF_TX = 11;     // SoftwareSerial pins to the DFPlayer Mini

// ---- Objects for the audio and climate modules -----------------------------
SoftwareSerial mp3Serial(DF_TX, DF_RX);   // software serial port for the MP3 player
DFRobotDFPlayerMini myMP3;                // the MP3 player itself

#define DHTTYPE DHT11                      // we use a DHT11 (not DHT22)
DHT dht(DHT_PIN, DHTTYPE);                 // the temperature/humidity sensor

// ---- Tunable thresholds (distances are in centimetres) ---------------------
const int OPEN_DIST = 20;        // front sensor: a hand closer than 20 cm opens the lid
const int HOLD_DIST = 30;        // inner sensor: object within 30 cm keeps the lid open
const int FULL_DIST = 6;         // 6 cm or less of free space = the bin is full
const int EMPTY_BIN_DIST = 22;   // distance to the bottom of an EMPTY bin (calibration)
const int PINCH_DIST = 3;        // anti-pinch: object within 3 cm while closing = reverse
const int CLOSE_DELAY = 1000;    // wait 1 s after the zone is clear before closing

// ---- Runtime state variables ----------------------------------------------
unsigned long zoneClearTime = 0; bool timerStarted = false;   // timing for the auto-close delay
bool isFull = false; bool isMuted = false;                    // bin-full flag; audio muted flag
int currentVolume = 25; int currentWelcomeTrack = 2; int currentAngryTrack = 6;  // volume + which audio clip plays next
unsigned long lastSecurityCheck = 0; const unsigned long SECURITY_INTERVAL = 5000;  // run the climate/tamper check every 5 s

// ---- Fire cool-down (stops the alarm re-firing on the sensor's heat lag) ----
bool isFireCooldown = false; unsigned long fireCooldownStart = 0;
const unsigned long FIRE_COOLDOWN_TIME = 180000;   // 180000 ms = 3 minutes

// ---- Who triggered the last lid opening (sent to the gateway for the log) ---
//  'H' = hand/sensor, 'T' = Telegram, 'P' = physical button, 'U' = unknown/none
char triggerSource = 'U';

// ============================================================================
//  WATCHDOG INTERRUPT  (the self-recovery safety net)
//  If the main program ever freezes, the watchdog fires this routine. We mark
//  a "crash happened" flag in EEPROM, then force a clean HARDWARE reset so the
//  bin restarts itself with no human needed.
// ============================================================================
ISR(WDT_vect) {
  EEPROM.write(0, 1);              // remember that this restart was caused by a crash
  WDTCSR = (1<<WDCE) | (1<<WDE);   // unlock the watchdog control register
  WDTCSR = (1<<WDE);              // switch watchdog to "reset" mode...
  while(1);                        // ...and hang here so it forces the reset
}

// ============================================================================
//  SETUP  (runs once at power-on / after every reset)
// ============================================================================
void setup() {
  Serial.begin(9600);             // hardware serial = the M2M link to the ESP8266
  mp3Serial.begin(9600);          // software serial = the MP3 player
  dht.begin();                    // start the temperature sensor

  // Configure every pin as input or output
  pinMode(TRIG_1, OUTPUT); pinMode(ECHO_1, INPUT);
  pinMode(TRIG_2, OUTPUT); pinMode(ECHO_2, INPUT);
  pinMode(AIN1, OUTPUT);   pinMode(AIN2, OUTPUT);
  pinMode(PWMA, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP); pinMode(BUSY_PIN, INPUT);  // button uses internal pull-up (pressed = LOW)

  // Did we restart because of a crash? Read the flag, then clear it for next time.
  bool wasCrash = (EEPROM.read(0) == 1);
  EEPROM.write(0, 0);
  delay(1500);                    // give the modules time to power up

  // Start the audio module. begin(serial, ack=true, reset=false): reset=false
  // is deliberate — resetting the DFPlayer here made it unreliable.
  if (myMP3.begin(mp3Serial, true, false)) {
    myMP3.volume(currentVolume);
    delay(500);
    if (!wasCrash) {              // only play the boot jingle on a NORMAL start...
      myMP3.playMp3Folder(1);    // ...so a silent crash-recovery is invisible to the public
      delay(1500);
      unsigned long waitFinish = millis();
      while(digitalRead(BUSY_PIN) == LOW && millis() - waitFinish < 10000) { delay(50); }  // wait until the clip ends (BUSY HIGH)
    }
  }

  // Safety close on boot: drive the lid shut, but watch for a trapped hand.
  digitalWrite(AIN1, LOW); digitalWrite(AIN2, HIGH); analogWrite(PWMA, 255);  // motor = closing direction, full speed
  unsigned long startClose = millis();
  bool startupInterrupted = false;

  while (millis() - startClose < 1900) {          // run the motor for up to 1.9 s
    long dist2 = getDistance(TRIG_2, ECHO_2);     // check the inner sensor
    if (dist2 > 0 && dist2 < PINCH_DIST) {        // something is in the way!
      startupInterrupted = true;
      break;                                       // stop closing immediately
    }
    delay(10);
  }

  actuatorStop();                 // cut motor power

  // Arm the watchdog in INTERRUPT mode (~8 s). cli()/sei() disable/enable
  // interrupts so the register write is not corrupted half-way.
  cli();
  wdt_reset();
  WDTCSR |= (1<<WDCE) | (1<<WDE);                       // enable change
  WDTCSR = (1<<WDIE) | (1<<WDE) | (1<<WDP3) | (1<<WDP0); // interrupt mode + ~8 s timeout
  sei();

  Serial.println(F("SYS:BOOT"));  // tell the gateway "I just rebooted" -> it resyncs its state

  // If a hand interrupted the boot-close, open instead of sitting idle.
  if (startupInterrupted) {
    currentState = OPENING;
    triggerSource = 'U';
  } else {
    currentState = IDLE;
  }
}

// ============================================================================
//  COMMANDS FROM THE GATEWAY  (single letters arriving over the serial link)
//  The ESP8266 sends one character; this decodes it into an action.
// ============================================================================
void processCommand(char cmd) {
  if (cmd == '+') {                         // volume UP
    isMuted = false;                        // changing volume un-mutes
    currentVolume = constrain(currentVolume + 2, 0, 30);   // keep within 0..30
    myMP3.volume(currentVolume);
    Serial.print(F("VOL:")); Serial.println(map(currentVolume, 0, 30, 0, 100));  // report volume back as 0..100 %
  }
  if (cmd == '-') {                         // volume DOWN
    isMuted = false;                        // changing volume un-mutes
    currentVolume = constrain(currentVolume - 2, 0, 30);
    myMP3.volume(currentVolume);
    Serial.print(F("VOL:")); Serial.println(map(currentVolume, 0, 30, 0, 100));
  }
  if (cmd == 'm') {                         // toggle MUTE
    isMuted = !isMuted;
    if (isMuted) { myMP3.volume(0); } else { myMP3.volume(currentVolume); }
  }
  if (cmd == 'o' && currentState != FIRE_ALARM) {   // remote OPEN (ignored during a fire)
    triggerSource = 'T';                    // 'T' = opened from Telegram
    currentState = OPENING;
  }
  if ((cmd == 'c' || cmd == 'o') && currentState == FIRE_ALARM) {  // CLEAR a fire alarm (open + cool down); Telegram Open clears too
    myMP3.stop();
    if (isMuted) myMP3.volume(0); else myMP3.volume(currentVolume);
    triggerSource = 'T';
    currentState = OPENING;
    isFireCooldown = true; fireCooldownStart = millis();   // start the 3-min cool-down
    Serial.println(F("SYS:COOLDOWN"));
  }
  if (cmd == 'F') {            // hidden self-test: drive the real fire-alarm path
    triggerFireAlarm();
  }
  if (cmd == 'r') {           // remote software reset of THIS board
    myMP3.stop();
    void (*resetFunc)(void) = 0;   // a function pointer to address 0...
    resetFunc();                    // ...calling it jumps to the reset vector
  }
}

// ============================================================================
//  MAIN LOOP  (runs forever; this is the heart of the FSM)
// ============================================================================
void loop() {
  wdt_reset();                    // "pet" the watchdog every loop so it knows we're alive

  if (Serial.available() > 0) {   // a command arrived from the gateway?
    processCommand(Serial.read());
  }

  // ---- Physical button handling (behaviour depends on the current state) ----
  if (checkButton()) {
    if (currentState == FIRE_ALARM) {        // button during a fire = clear it + cool down
      myMP3.stop();
      if (isMuted) myMP3.volume(0); else myMP3.volume(currentVolume);
      triggerSource = 'P';
      currentState = OPENING;
      isFireCooldown = true; fireCooldownStart = millis();
      Serial.println(F("SYS:COOLDOWN"));
    }
    else if (currentState == IDLE || currentState == CLOSING || currentState == FULL_LOCKED) {
      myMP3.stop();                          // button when idle/closing/locked = open
      triggerSource = 'P';
      currentState = OPENING;
    }
    else if (currentState == OPEN) { zoneClearTime = millis(); timerStarted = true; }  // button while open = restart close timer
  }

  // ---- Periodic climate + tamper check (also runs during a fire so the
  //      heartbeat keeps flowing; the fire re-trigger itself is guarded) ------
  if (millis() - lastSecurityCheck >= SECURITY_INTERVAL) {
    performSecurityCheck(); lastSecurityCheck = millis();
  }

  // ---- The finite-state machine -------------------------------------------
  switch (currentState) {
    case IDLE: {                                   // waiting; watch the front sensor
      long dist1 = getDistance(TRIG_1, ECHO_1);
      if (dist1 > 0 && dist1 < OPEN_DIST) {        // a user is close -> open
        triggerSource = 'H';
        currentState = OPENING;
      }
      delay(150); break;
    }

    case OPENING: {                                // actively raising the lid
      Serial.print(F("LID:OPEN:"));                // tell the gateway the lid opened...
      Serial.println(triggerSource);               // ...and who/what triggered it
      triggerSource = 'U';
      digitalWrite(AIN1, HIGH); digitalWrite(AIN2, LOW); analogWrite(PWMA, 255);  // motor = opening direction
      delay(2500); actuatorStop(); currentState = OPEN; timerStarted = false;     // open for 2.5 s, then hold
      if (!isMuted) {                              // play a friendly welcome clip (the "nudge")
        myMP3.playMp3Folder(currentWelcomeTrack);
        currentWelcomeTrack++; if (currentWelcomeTrack > 4) currentWelcomeTrack = 2;  // cycle clips 2..4
      }
      break;
    }

    case OPEN: {                                   // lid is up; decide when to close
      long dist2 = getDistance(TRIG_2, ECHO_2);
      if (dist2 > 0 && dist2 < HOLD_DIST) { timerStarted = false; }  // someone still using it -> keep open
      else {
        if (!timerStarted) { zoneClearTime = millis(); timerStarted = true; }   // zone just cleared -> start timer
        if (millis() - zoneClearTime >= CLOSE_DELAY) {                           // 1 s elapsed...
          if (digitalRead(BUSY_PIN) == HIGH || millis() - zoneClearTime >= 4000) {  // ...and audio finished (or 4 s safety cap)
            currentState = CLOSING;
          }
        }
      }
      delay(150); break;
    }

    case CLOSING: {                                // lowering the lid WITH active anti-pinch
      digitalWrite(AIN1, LOW); digitalWrite(AIN2, HIGH); analogWrite(PWMA, 255);  // motor = closing direction
      unsigned long startClose = millis(); bool interrupted = false;

      while(millis() - startClose < 1900) {        // close for up to 1.9 s
        if (checkButton()) { interrupted = true; break; }       // button pressed -> abort close
        long dist2 = getDistance(TRIG_2, ECHO_2);
        if (dist2 > 0 && dist2 < PINCH_DIST) {     // ANTI-PINCH: a hand is within 3 cm
          interrupted = true;
          break;                                    // stop and re-open
        }
        delay(10);
      }

      actuatorStop();

      if (interrupted) {                            // something blocked the close -> re-open
        triggerSource = 'H';
        currentState = OPENING;
        break;
      }

      if (checkIfBinIsFull()) {                     // closed OK -> is it now full?
        isFull = true;
        Serial.println(F("ALARM:FULL")); Serial.println(F("LID:CLOSED"));  // tell the gateway: full + closed
        currentState = FULL_LOCKED;
      } else {
        isFull = false;
        Serial.println(F("LID:CLOSED")); currentState = IDLE;              // tell the gateway: closed, back to idle
      }
      break;
    }

    case FULL_LOCKED: {                            // bin is full; lid stays shut
      long dist1 = getDistance(TRIG_1, ECHO_1);
      if (dist1 > 0 && dist1 < OPEN_DIST && !isMuted) {   // someone approaches -> play an "I'm full" clip
        myMP3.playMp3Folder(currentAngryTrack);
        currentAngryTrack++; if (currentAngryTrack > 8) currentAngryTrack = 6;  // cycle clips 6..8
        unsigned long busyWait = millis(); delay(500);
        while(digitalRead(BUSY_PIN) == LOW && millis() - busyWait < 5000) { delay(50); }  // wait for the clip to finish
      }
      delay(150); break;
    }

    case FIRE_ALARM: {                             // fire detected; keep the siren going
      if (digitalRead(BUSY_PIN) == HIGH) { myMP3.playMp3Folder(9); delay(500); }  // replay the alarm clip when it ends
      break;
    }
  }
}

// ============================================================================
//  FIRE ALARM  (the deliberate "seal the lid" safety inversion)
//  Called from the climate check (>=35 C) or the hidden /firetest command.
// ============================================================================
void triggerFireAlarm() {
  Serial.println(F("ALARM:FIRE"));                 // tell the gateway -> broadcast alert to all users
  bool needsClosing = (currentState == OPEN || currentState == OPENING || currentState == CLOSING);  // is the lid open right now?
  myMP3.volume(30); myMP3.playMp3Folder(9); currentState = FIRE_ALARM;   // full volume siren (ignores mute), enter fire state
  delay(2000);
  if (needsClosing) {                              // if the lid was open, SEAL it to starve the fire of oxygen
    digitalWrite(AIN1, LOW); digitalWrite(AIN2, HIGH); analogWrite(PWMA, 255);
    delay(1900); actuatorStop();
  }
}

// ============================================================================
//  PERIODIC CHECK  (temperature -> fire, and tamper/vandalism detection)
// ============================================================================
void performSecurityCheck() {
  float t = dht.readTemperature();
  float h = dht.readHumidity();

  if (isnan(t)) return;                            // bad read -> skip this cycle

  Serial.print(F("TEMP:")); Serial.println(t);     // stream temperature to the gateway (-> ThingSpeak)
  if (!isnan(h)) {
    Serial.print(F("HUM:")); Serial.println(h);    // stream humidity too
  }

  if (isFireCooldown) {                            // recently cleared a fire? wait out the 3-min cool-down
    if (millis() - fireCooldownStart < FIRE_COOLDOWN_TIME) return;
    else isFireCooldown = false;
  }

  if (t >= 35.0 && currentState != FIRE_ALARM) {   // FIRE THRESHOLD: 35 C (skip if already alarming)
    triggerFireAlarm();
  }

  // TAMPER / VANDALISM: when idle or locked, the inner sensor should "see" the
  // bin floor. A 999 (no echo) or a reading far past the floor means the lid
  // was removed or the bin was tampered with.
  if (currentState == IDLE || currentState == FULL_LOCKED) {
    long d2 = getDistance(TRIG_2, ECHO_2);
    if (d2 == 999 || d2 > EMPTY_BIN_DIST + 20) { Serial.println(F("ALARM:VANDAL")); }
  }
}

// ============================================================================
//  FILL MEASUREMENT  (averages 3 reads, maps distance -> percentage)
// ============================================================================
bool checkIfBinIsFull() {
  long sumDistance = 0; int validMeasurements = 0;
  for (int i = 0; i < 3; i++) {                    // take 3 readings to reduce noise
    long d = getDistance(TRIG_2, ECHO_2);
    if (d > 0 && d < EMPTY_BIN_DIST + 10) { sumDistance += d; validMeasurements++; }  // ignore obviously wrong reads
    delay(50);
  }
  if (validMeasurements == 0) return false;        // no good reads -> assume not full
  long averageDistance = sumDistance / validMeasurements;
  // Map distance to a 0..100 % fill: EMPTY_BIN_DIST = 0 %, FULL_DIST = 100 %.
  int fillPercent = map(averageDistance, EMPTY_BIN_DIST, FULL_DIST, 0, 100);
  fillPercent = constrain(fillPercent, 0, 100);
  Serial.print(F("FILL:")); Serial.println(fillPercent);   // stream fill % to the gateway
  return (fillPercent >= 90 || averageDistance <= FULL_DIST);   // "full" at 90 % or more
}

// ---- Stop the lid motor (both directions off, speed 0) ---------------------
void actuatorStop() { digitalWrite(AIN1, LOW); digitalWrite(AIN2, LOW); analogWrite(PWMA, 0); }

// ============================================================================
//  ULTRASONIC DISTANCE  (HC-SR04: send a 10 us pulse, time the echo)
//  Returns distance in cm, or 999 if no echo came back (timeout).
// ============================================================================
long getDistance(int trigPin, int echoPin) {
  digitalWrite(trigPin, LOW); delayMicroseconds(2);
  digitalWrite(trigPin, HIGH); delayMicroseconds(10);   // 10 us trigger pulse
  digitalWrite(trigPin, LOW);
  long duration = pulseIn(echoPin, HIGH, 30000);        // measure echo time (30 ms timeout)
  if (duration == 0) return 999;                        // no echo -> "very far / error"
  return duration / 58;                                 // convert microseconds to centimetres
}

// ============================================================================
//  BUTTON READ  (debounced; returns true once per press)
// ============================================================================
bool checkButton() {
  if (digitalRead(BUTTON_PIN) == LOW) {            // pressed (pull-up: LOW = pressed)
    delay(50);                                      // debounce wait
    if (digitalRead(BUTTON_PIN) == LOW) {           // still pressed -> a real press
      unsigned long btnWait = millis();
      while(digitalRead(BUTTON_PIN) == LOW && millis() - btnWait < 2000) { delay(10); }  // wait for release (max 2 s)
      return true;
    }
  }
  return false;
}
