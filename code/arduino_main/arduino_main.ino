// SmartBin v0.3 — Add DFPlayer Mini audio + DHT11 climate monitor.
// DFPlayer plays welcome tracks on open, complaint tracks when bin is full.
// DHT11 checks temperature every 5s; basic threshold for future fire alarm.
// BUSY pin (pin 4) prevents lid closing while audio is playing.

#include <SoftwareSerial.h>
#include <DFRobotDFPlayerMini.h>
#include <DHT.h>

const int TRIG_1 = A0; const int ECHO_1 = A1;
const int TRIG_2 = A2; const int ECHO_2 = A3;
const int AIN1 = 8; const int AIN2 = 9; const int PWMA = 5;
const int BUSY_PIN = 4;
const int DHT_PIN  = 7;

const int DF_RX = 10; const int DF_TX = 11;
SoftwareSerial mp3Serial(DF_TX, DF_RX);
DFRobotDFPlayerMini myMP3;

#define DHTTYPE DHT11
DHT dht(DHT_PIN, DHTTYPE);

const unsigned long OPEN_TIME_MS  = 2500;
const unsigned long CLOSE_TIME_MS = 1900;
const int OPEN_DIST  = 20;
const int HOLD_DIST  = 30;
const int FULL_DIST  = 6;
const int EMPTY_DIST = 40;
const int CLOSE_DELAY = 1000;

int currentVolume = 15;
int welcomeTrack  = 2;
int angryTrack    = 6;

enum State { IDLE, OPENING, OPEN, CLOSING, FULL_LOCKED };
State currentState = IDLE;

unsigned long zoneClearTime = 0;
bool timerStarted = false;
unsigned long lastTempCheck = 0;

void actuatorExtend()  { digitalWrite(AIN1, HIGH); digitalWrite(AIN2, LOW);  analogWrite(PWMA, 255); }
void actuatorRetract() { digitalWrite(AIN1, LOW);  digitalWrite(AIN2, HIGH); analogWrite(PWMA, 255); }
void actuatorStop()    { digitalWrite(AIN1, LOW);  digitalWrite(AIN2, LOW);  analogWrite(PWMA, 0);   }

long readDistance(int trig, int echo) {
  digitalWrite(trig, LOW);  delayMicroseconds(2);
  digitalWrite(trig, HIGH); delayMicroseconds(10);
  digitalWrite(trig, LOW);
  long d = pulseIn(echo, HIGH, 30000);
  return d > 0 ? d / 58 : 999;
}

bool checkIfFull() {
  long sum = 0; int n = 0;
  for (int i = 0; i < 3; i++) {
    long d = readDistance(TRIG_2, ECHO_2);
    if (d > 0 && d < EMPTY_DIST + 10) { sum += d; n++; }
    delay(50);
  }
  if (n == 0) return false;
  long avg = sum / n;
  int pct = map(avg, EMPTY_DIST, FULL_DIST, 0, 100);
  pct = constrain(pct, 0, 100);
  Serial.print("FILL:"); Serial.println(pct);
  return pct >= 90 || avg <= FULL_DIST;
}

void setup() {
  Serial.begin(9600);
  mp3Serial.begin(9600);
  dht.begin();
  pinMode(TRIG_1, OUTPUT); pinMode(ECHO_1, INPUT);
  pinMode(TRIG_2, OUTPUT); pinMode(ECHO_2, INPUT);
  pinMode(AIN1, OUTPUT); pinMode(AIN2, OUTPUT); pinMode(PWMA, OUTPUT);
  pinMode(BUSY_PIN, INPUT);

  if (myMP3.begin(mp3Serial)) {
    myMP3.volume(currentVolume);
    delay(500);
    myMP3.playMp3Folder(1);  // startup jingle
    delay(1500);
  }

  actuatorRetract();
  delay(CLOSE_TIME_MS);
  actuatorStop();
  currentState = IDLE;
}

void loop() {
  if (Serial.available() > 0) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (cmd == "OPEN_BIN" && currentState == IDLE) { currentState = OPENING; }
  }

  // Temperature check every 5s
  if (millis() - lastTempCheck >= 5000) {
    float t = dht.readTemperature();
    if (!isnan(t)) { Serial.print("TEMP:"); Serial.println(t); }
    lastTempCheck = millis();
  }

  switch (currentState) {
    case IDLE: {
      long d1 = readDistance(TRIG_1, ECHO_1);
      if (d1 > 0 && d1 < OPEN_DIST) currentState = OPENING;
      delay(150); break;
    }
    case OPENING: {
      Serial.println("LID:OPEN");
      actuatorExtend();
      delay(OPEN_TIME_MS);
      actuatorStop();
      myMP3.playMp3Folder(welcomeTrack);
      welcomeTrack++;
      if (welcomeTrack > 4) welcomeTrack = 2;
      currentState = OPEN;
      timerStarted = false;
      break;
    }
    case OPEN: {
      long d2 = readDistance(TRIG_2, ECHO_2);
      if (d2 > 0 && d2 < HOLD_DIST) {
        timerStarted = false;
      } else {
        if (!timerStarted) { zoneClearTime = millis(); timerStarted = true; }
        if (millis() - zoneClearTime >= CLOSE_DELAY) {
          if (digitalRead(BUSY_PIN) == HIGH) currentState = CLOSING;
        }
      }
      delay(150); break;
    }
    case CLOSING: {
      actuatorRetract();
      delay(CLOSE_TIME_MS);
      actuatorStop();
      Serial.println("LID:CLOSED");
      currentState = checkIfFull() ? FULL_LOCKED : IDLE;
      break;
    }
    case FULL_LOCKED: {
      long d1 = readDistance(TRIG_1, ECHO_1);
      if (d1 > 0 && d1 < OPEN_DIST) {
        myMP3.playMp3Folder(angryTrack);
        angryTrack++;
        if (angryTrack > 8) angryTrack = 6;
        delay(500);
      }
      delay(150); break;
    }
  }
}
