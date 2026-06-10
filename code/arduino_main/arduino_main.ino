// SmartBin v1.1 — Dual-controller architecture + watchdog crash recovery.
// Arduino Uno handles all hardware; NodeMCU ESP8266 handles all networking.
// Communication: Hardware Serial pins 0/1 (cross-wired, 9600 baud).
// Arduino sends event strings (LID:OPEN, LID:CLOSED, FILL:%, TEMP:, ALARM:*).
// NodeMCU sends single-char commands: 'o' open, 'm' mute, '+'/'-' volume, 'r' reset.
// Watchdog: 4 s timeout. ISR writes crash flag to EEPROM[0] before reset.
//
// IMPORTANT: disconnect pins 0/1 before uploading firmware via USB.

#include <SoftwareSerial.h>
#include <DFRobotDFPlayerMini.h>
#include <DHT.h>
#include <EEPROM.h>
#include <avr/wdt.h>

#define CRASH_FLAG_ADDR 0
#define CRASH_MAGIC     0xAB

enum State { IDLE, OPENING, OPEN, CLOSING, FULL_LOCKED };
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

const int OPEN_DIST   = 20;
const int HOLD_DIST   = 30;
const int FULL_DIST   = 6;
const int EMPTY_DIST  = 40;
const int CLOSE_DELAY = 1000;

bool isMuted = false;
int  currentVolume    = 15;
int  welcomeTrack     = 2;
int  angryTrack       = 6;
unsigned long zoneClearTime = 0;
bool timerStarted = false;
unsigned long lastSecCheck = 0;

void actuatorExtend()  { digitalWrite(AIN1, HIGH); digitalWrite(AIN2, LOW);  analogWrite(PWMA, 255); }
void actuatorRetract() { digitalWrite(AIN1, LOW);  digitalWrite(AIN2, HIGH); analogWrite(PWMA, 255); }
void actuatorStop()    { digitalWrite(AIN1, LOW);  digitalWrite(AIN2, LOW);  analogWrite(PWMA, 0);   }

long getDistance(int trig, int echo) {
  digitalWrite(trig, LOW);  delayMicroseconds(2);
  digitalWrite(trig, HIGH); delayMicroseconds(10);
  digitalWrite(trig, LOW);
  long d = pulseIn(echo, HIGH, 30000);
  return d > 0 ? d / 58 : 999;
}

bool checkButton() {
  if (digitalRead(BUTTON_PIN) == LOW) {
    delay(50);
    if (digitalRead(BUTTON_PIN) == LOW) {
      while (digitalRead(BUTTON_PIN) == LOW) delay(10);
      return true;
    }
  }
  return false;
}

bool checkIfFull() {
  long sum = 0; int n = 0;
  for (int i = 0; i < 3; i++) {
    long d = getDistance(TRIG_2, ECHO_2);
    if (d > 0 && d < EMPTY_DIST + 10) { sum += d; n++; }
    delay(50);
  }
  if (n == 0) return false;
  long avg = sum / n;
  int pct = constrain(map(avg, EMPTY_DIST, FULL_DIST, 0, 100), 0, 100);
  Serial.print(F("FILL:")); Serial.println(pct);
  return pct >= 90 || avg <= FULL_DIST;
}

void processCommand(char cmd) {
  if (cmd == '+') { currentVolume = constrain(currentVolume + 2, 0, 30); if (!isMuted) myMP3.volume(currentVolume); Serial.print(F("VOL:")); Serial.println(map(currentVolume, 0, 30, 0, 100)); }
  if (cmd == '-') { currentVolume = constrain(currentVolume - 2, 0, 30); if (!isMuted) myMP3.volume(currentVolume); Serial.print(F("VOL:")); Serial.println(map(currentVolume, 0, 30, 0, 100)); }
  if (cmd == 'm') { isMuted = !isMuted; myMP3.volume(isMuted ? 0 : currentVolume); }
  if (cmd == 'o' && currentState == IDLE) currentState = OPENING;
  if (cmd == 'r') { void (*reset)(void) = 0; reset(); }
}

ISR(WDT_vect) {
  EEPROM.write(CRASH_FLAG_ADDR, CRASH_MAGIC);
}

void setup() {
  wdt_disable();
  Serial.begin(9600);
  mp3Serial.begin(9600);
  dht.begin();
  pinMode(TRIG_1, OUTPUT); pinMode(ECHO_1, INPUT);
  pinMode(TRIG_2, OUTPUT); pinMode(ECHO_2, INPUT);
  pinMode(AIN1, OUTPUT); pinMode(AIN2, OUTPUT); pinMode(PWMA, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP); pinMode(BUSY_PIN, INPUT);
  delay(1000);
  if (EEPROM.read(CRASH_FLAG_ADDR) == CRASH_MAGIC) {
    EEPROM.write(CRASH_FLAG_ADDR, 0x00);
    Serial.println(F("SYS:CRASH"));
  }
  if (myMP3.begin(mp3Serial)) { myMP3.volume(currentVolume); delay(500); myMP3.playMp3Folder(1); delay(1500); }
  actuatorRetract(); delay(1900); actuatorStop();
  currentState = IDLE;
  Serial.println(F("SYS:BOOT"));
  wdt_enable(WDTO_4S);
}

void loop() {
  wdt_reset();
  if (Serial.available() > 0) processCommand(Serial.read());

  if (checkButton() && (currentState == IDLE || currentState == CLOSING)) {
    myMP3.stop();
    currentState = OPENING;
  }

  if (millis() - lastSecCheck >= 5000) {
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (!isnan(t)) { Serial.print(F("TEMP:")); Serial.println(t); }
    if (!isnan(h)) { Serial.print(F("HUM:"));  Serial.println(h); }
    lastSecCheck = millis();
  }

  switch (currentState) {
    case IDLE: {
      long d1 = getDistance(TRIG_1, ECHO_1);
      if (d1 > 0 && d1 < OPEN_DIST) currentState = OPENING;
      delay(150); break;
    }
    case OPENING: {
      Serial.println(F("LID:OPEN"));
      actuatorExtend(); delay(2500); actuatorStop();
      if (!isMuted) {
        myMP3.playMp3Folder(welcomeTrack); welcomeTrack++; if (welcomeTrack > 4) welcomeTrack = 2;
        // DFPlayer needs ~200 ms to assert BUSY after a play command.
        // Without this wait, OPEN state sees BUSY=HIGH immediately and starts the close timer.
        delay(250);
      }
      currentState = OPEN; timerStarted = false;
      break;
    }
    case OPEN: {
      long d2 = getDistance(TRIG_2, ECHO_2);
      if (d2 > 0 && d2 < HOLD_DIST) { timerStarted = false; }
      else {
        if (!timerStarted) { zoneClearTime = millis(); timerStarted = true; }
        if (millis() - zoneClearTime >= CLOSE_DELAY && digitalRead(BUSY_PIN) == HIGH) currentState = CLOSING;
      }
      delay(150); break;
    }
    case CLOSING: {
      actuatorRetract(); delay(1900); actuatorStop();
      Serial.println(F("LID:CLOSED"));
      currentState = checkIfFull() ? FULL_LOCKED : IDLE;
      if (currentState == FULL_LOCKED) { Serial.println(F("ALARM:FULL")); }
      break;
    }
    case FULL_LOCKED: {
      long d1 = getDistance(TRIG_1, ECHO_1);
      if (d1 > 0 && d1 < OPEN_DIST && !isMuted) {
        myMP3.playMp3Folder(angryTrack); angryTrack++; if (angryTrack > 8) angryTrack = 6;
        delay(500);
      }
      delay(150); break;
    }
  }
}
