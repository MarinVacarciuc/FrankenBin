// SMARTBIN v3.3 — Production Mode (from 14.06.2026v3.docx)
// 6-state FSM with fire alarm, anti-pinch, watchdog interrupt mode.
// M2M: Hardware Serial pins 0/1 (cross-wired, 9600 baud).
//
// IMPORTANT: disconnect pins 0/1 before uploading firmware via USB.

#include <SoftwareSerial.h>
#include <DFRobotDFPlayerMini.h>
#include <DHT.h>
#include <avr/wdt.h>
#include <EEPROM.h>

enum State { IDLE, OPENING, OPEN, CLOSING, FULL_LOCKED, FIRE_ALARM };
State currentState = IDLE;

const int TRIG_1 = A0; const int ECHO_1 = A1;
const int TRIG_2 = A2; const int ECHO_2 = A3;
const int AIN1 = 8; const int AIN2 = 9; const int PWMA = 5;
const int BUTTON_PIN = 12; const int BUSY_PIN = 4; const int DHT_PIN = 7;
const int DF_RX = 10; const int DF_TX = 11;

SoftwareSerial mp3Serial(DF_TX, DF_RX);
DFRobotDFPlayerMini myMP3;

#define DHTTYPE DHT11
DHT dht(DHT_PIN, DHTTYPE);

const int OPEN_DIST = 20;
const int HOLD_DIST = 30;
const int FULL_DIST = 6;
const int EMPTY_BIN_DIST = 40;
const int PINCH_DIST = 4;
const int CLOSE_DELAY = 1000;

unsigned long zoneClearTime = 0; bool timerStarted = false;
bool isFull = false; bool isMuted = false;
int currentVolume = 15; int currentWelcomeTrack = 2; int currentAngryTrack = 6;
unsigned long lastSecurityCheck = 0; const unsigned long SECURITY_INTERVAL = 5000;

bool isFireCooldown = false; unsigned long fireCooldownStart = 0;
const unsigned long FIRE_COOLDOWN_TIME = 180000;

char triggerSource = 'U';

ISR(WDT_vect) {
  EEPROM.write(0, 1);
  WDTCSR = (1<<WDCE) | (1<<WDE);
  WDTCSR = (1<<WDE);
  while(1);
}

void setup() {
  Serial.begin(9600);
  mp3Serial.begin(9600);
  dht.begin();

  pinMode(TRIG_1, OUTPUT); pinMode(ECHO_1, INPUT);
  pinMode(TRIG_2, OUTPUT); pinMode(ECHO_2, INPUT);
  pinMode(AIN1, OUTPUT);   pinMode(AIN2, OUTPUT);
  pinMode(PWMA, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP); pinMode(BUSY_PIN, INPUT);

  bool wasCrash = (EEPROM.read(0) == 1);
  EEPROM.write(0, 0);
  delay(1500);

  if (myMP3.begin(mp3Serial, true, false)) {
    myMP3.volume(currentVolume);
    delay(500);
    if (!wasCrash) {
      myMP3.playMp3Folder(1);
      delay(1500);
      unsigned long waitFinish = millis();
      while(digitalRead(BUSY_PIN) == LOW && millis() - waitFinish < 10000) { delay(50); }
    }
  }

  digitalWrite(AIN1, LOW); digitalWrite(AIN2, HIGH); analogWrite(PWMA, 255);
  unsigned long startClose = millis();
  bool startupInterrupted = false;

  while (millis() - startClose < 1900) {
    long dist2 = getDistance(TRIG_2, ECHO_2);
    if (dist2 > 0 && dist2 < PINCH_DIST) {
      startupInterrupted = true;
      break;
    }
    delay(10);
  }

  actuatorStop();

  cli();
  wdt_reset();
  WDTCSR |= (1<<WDCE) | (1<<WDE);
  WDTCSR = (1<<WDIE) | (1<<WDE) | (1<<WDP3) | (1<<WDP0);
  sei();

  Serial.println(F("SYS:BOOT"));

  if (startupInterrupted) {
    currentState = OPENING;
    triggerSource = 'U';
  } else {
    currentState = IDLE;
  }
}

void processCommand(char cmd) {
  if (cmd == '+') {
    isMuted = false;                 // changing volume un-mutes
    currentVolume = constrain(currentVolume + 2, 0, 30);
    myMP3.volume(currentVolume);
    Serial.print(F("VOL:")); Serial.println(map(currentVolume, 0, 30, 0, 100));
  }
  if (cmd == '-') {
    isMuted = false;                 // changing volume un-mutes
    currentVolume = constrain(currentVolume - 2, 0, 30);
    myMP3.volume(currentVolume);
    Serial.print(F("VOL:")); Serial.println(map(currentVolume, 0, 30, 0, 100));
  }
  if (cmd == 'm') {
    isMuted = !isMuted;
    if (isMuted) { myMP3.volume(0); } else { myMP3.volume(currentVolume); }
  }
  if (cmd == 'o' && currentState != FIRE_ALARM) {
    triggerSource = 'T';
    currentState = OPENING;
  }
  if (cmd == 'c' && currentState == FIRE_ALARM) {
    myMP3.stop();
    if (isMuted) myMP3.volume(0); else myMP3.volume(currentVolume);
    triggerSource = 'T';
    currentState = OPENING;
    isFireCooldown = true; fireCooldownStart = millis();
    Serial.println(F("SYS:COOLDOWN"));
  }
  if (cmd == 'F') {            // hidden self-test: drive the real fire-alarm path
    triggerFireAlarm();
  }
  if (cmd == 'r') {
    myMP3.stop();
    void (*resetFunc)(void) = 0;
    resetFunc();
  }
}

void loop() {
  wdt_reset();

  if (Serial.available() > 0) {
    processCommand(Serial.read());
  }

  if (checkButton()) {
    if (currentState == FIRE_ALARM) {
      myMP3.stop();
      if (isMuted) myMP3.volume(0); else myMP3.volume(currentVolume);
      triggerSource = 'P';
      currentState = OPENING;
      isFireCooldown = true; fireCooldownStart = millis();
      Serial.println(F("SYS:COOLDOWN"));
    }
    else if (currentState == IDLE || currentState == CLOSING || currentState == FULL_LOCKED) {
      myMP3.stop();
      triggerSource = 'P';
      currentState = OPENING;
    }
    else if (currentState == OPEN) { zoneClearTime = millis(); timerStarted = true; }
  }

  if (currentState != FIRE_ALARM && millis() - lastSecurityCheck >= SECURITY_INTERVAL) {
    performSecurityCheck(); lastSecurityCheck = millis();
  }

  switch (currentState) {
    case IDLE: {
      long dist1 = getDistance(TRIG_1, ECHO_1);
      if (dist1 > 0 && dist1 < OPEN_DIST) {
        triggerSource = 'H';
        currentState = OPENING;
      }
      delay(150); break;
    }

    case OPENING: {
      Serial.print(F("LID:OPEN:"));
      Serial.println(triggerSource);
      triggerSource = 'U';
      digitalWrite(AIN1, HIGH); digitalWrite(AIN2, LOW); analogWrite(PWMA, 255);
      delay(2500); actuatorStop(); currentState = OPEN; timerStarted = false;
      if (!isMuted) {
        myMP3.playMp3Folder(currentWelcomeTrack);
        currentWelcomeTrack++; if (currentWelcomeTrack > 4) currentWelcomeTrack = 2;
      }
      break;
    }

    case OPEN: {
      long dist2 = getDistance(TRIG_2, ECHO_2);
      if (dist2 > 0 && dist2 < HOLD_DIST) { timerStarted = false; }
      else {
        if (!timerStarted) { zoneClearTime = millis(); timerStarted = true; }
        if (millis() - zoneClearTime >= CLOSE_DELAY) {
          if (digitalRead(BUSY_PIN) == HIGH || millis() - zoneClearTime >= 4000) {
            currentState = CLOSING;
          }
        }
      }
      delay(150); break;
    }

    case CLOSING: {
      digitalWrite(AIN1, LOW); digitalWrite(AIN2, HIGH); analogWrite(PWMA, 255);
      unsigned long startClose = millis(); bool interrupted = false;

      while(millis() - startClose < 1900) {
        if (checkButton()) { interrupted = true; break; }
        long dist2 = getDistance(TRIG_2, ECHO_2);
        if (dist2 > 0 && dist2 < PINCH_DIST) {
          interrupted = true;
          break;
        }
        delay(10);
      }

      actuatorStop();

      if (interrupted) {
        triggerSource = 'H';
        currentState = OPENING;
        break;
      }

      if (checkIfBinIsFull()) {
        isFull = true;
        Serial.println(F("ALARM:FULL")); Serial.println(F("LID:CLOSED"));
        currentState = FULL_LOCKED;
      } else {
        isFull = false;
        Serial.println(F("LID:CLOSED")); currentState = IDLE;
      }
      break;
    }

    case FULL_LOCKED: {
      long dist1 = getDistance(TRIG_1, ECHO_1);
      if (dist1 > 0 && dist1 < OPEN_DIST && !isMuted) {
        myMP3.playMp3Folder(currentAngryTrack);
        currentAngryTrack++; if (currentAngryTrack > 8) currentAngryTrack = 6;
        unsigned long busyWait = millis(); delay(500);
        while(digitalRead(BUSY_PIN) == LOW && millis() - busyWait < 5000) { delay(50); }
      }
      delay(150); break;
    }

    case FIRE_ALARM: {
      if (digitalRead(BUSY_PIN) == HIGH) { myMP3.playMp3Folder(9); delay(500); }
      break;
    }
  }
}

void triggerFireAlarm() {
  Serial.println(F("ALARM:FIRE"));
  bool needsClosing = (currentState == OPEN || currentState == OPENING || currentState == CLOSING);
  myMP3.volume(30); myMP3.playMp3Folder(9); currentState = FIRE_ALARM;
  delay(2000);
  if (needsClosing) {
    digitalWrite(AIN1, LOW); digitalWrite(AIN2, HIGH); analogWrite(PWMA, 255);
    delay(1900); actuatorStop();
  }
}

void performSecurityCheck() {
  float t = dht.readTemperature();
  float h = dht.readHumidity();

  if (isnan(t)) return;

  Serial.print(F("TEMP:")); Serial.println(t);
  if (!isnan(h)) {
    Serial.print(F("HUM:")); Serial.println(h);
  }

  if (isFireCooldown) {
    if (millis() - fireCooldownStart < FIRE_COOLDOWN_TIME) return;
    else isFireCooldown = false;
  }

  if (t >= 35.0) {
    triggerFireAlarm();
  }

  if (currentState == IDLE || currentState == FULL_LOCKED) {
    long d2 = getDistance(TRIG_2, ECHO_2);
    if (d2 == 999 || d2 > EMPTY_BIN_DIST + 20) { Serial.println(F("ALARM:VANDAL")); }
  }
}

bool checkIfBinIsFull() {
  long sumDistance = 0; int validMeasurements = 0;
  for (int i = 0; i < 3; i++) {
    long d = getDistance(TRIG_2, ECHO_2);
    if (d > 0 && d < EMPTY_BIN_DIST + 10) { sumDistance += d; validMeasurements++; }
    delay(50);
  }
  if (validMeasurements == 0) return false;
  long averageDistance = sumDistance / validMeasurements;
  int fillPercent = map(averageDistance, EMPTY_BIN_DIST, FULL_DIST, 0, 100);
  fillPercent = constrain(fillPercent, 0, 100);
  Serial.print(F("FILL:")); Serial.println(fillPercent);
  return (fillPercent >= 90 || averageDistance <= FULL_DIST);
}

void actuatorStop() { digitalWrite(AIN1, LOW); digitalWrite(AIN2, LOW); analogWrite(PWMA, 0); }

long getDistance(int trigPin, int echoPin) {
  digitalWrite(trigPin, LOW); delayMicroseconds(2);
  digitalWrite(trigPin, HIGH); delayMicroseconds(10);
  digitalWrite(trigPin, LOW);
  long duration = pulseIn(echoPin, HIGH, 30000);
  if (duration == 0) return 999;
  return duration / 58;
}

bool checkButton() {
  if (digitalRead(BUTTON_PIN) == LOW) {
    delay(50);
    if (digitalRead(BUTTON_PIN) == LOW) {
      unsigned long btnWait = millis();
      while(digitalRead(BUTTON_PIN) == LOW && millis() - btnWait < 2000) { delay(10); }
      return true;
    }
  }
  return false;
}
