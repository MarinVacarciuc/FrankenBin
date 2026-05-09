// SmartBin v0.2 — Switch from DVD stepper to KT0905 linear actuator + TB6612FNG.
// DVD motors lacked torque. Linear actuator gives reliable 25 mm stroke at 10 mm/s.
// Added 5-state FSM: IDLE → OPENING → OPEN → CLOSING → FULL.
// Second HC-SR04 added inside lid for fill-level measurement.

const int TRIG_1 = A0; const int ECHO_1 = A1;  // front sensor — hand detection
const int TRIG_2 = A2; const int ECHO_2 = A3;  // lid sensor  — fill / safety hold

const int AIN1 = 8;
const int AIN2 = 9;
const int PWMA = 5;

const unsigned long OPEN_TIME_MS  = 2300;
const unsigned long CLOSE_TIME_MS = 1900;
const int OPEN_DIST   = 30;
const int HOLD_DIST   = 70;
const int FULL_DIST   = 6;

enum State { IDLE, OPENING, OPEN, CLOSING, FULL };
State currentState = IDLE;
unsigned long stateStartTime = 0;

void actuatorExtend() { digitalWrite(AIN1, HIGH); digitalWrite(AIN2, LOW);  analogWrite(PWMA, 255); }
void actuatorRetract() { digitalWrite(AIN1, LOW);  digitalWrite(AIN2, HIGH); analogWrite(PWMA, 255); }
void actuatorStop()   { digitalWrite(AIN1, LOW);  digitalWrite(AIN2, LOW);  analogWrite(PWMA, 0);   }

long readDistance(int trig, int echo) {
  digitalWrite(trig, LOW);  delayMicroseconds(2);
  digitalWrite(trig, HIGH); delayMicroseconds(10);
  digitalWrite(trig, LOW);
  long d = pulseIn(echo, HIGH, 30000);
  return d > 0 ? d / 58 : 999;
}

bool isHandNear()    { long d = readDistance(TRIG_1, ECHO_1); return d > 0 && d < OPEN_DIST; }
bool isPersonNear()  { long d = readDistance(TRIG_2, ECHO_2); return d > 0 && d < HOLD_DIST; }
bool isBinFull() {
  long sum = 0; int n = 0;
  for (int i = 0; i < 3; i++) {
    long d = readDistance(TRIG_2, ECHO_2);
    if (d > 0 && d < 50) { sum += d; n++; }
    delay(50);
  }
  return n > 0 && (sum / n) < FULL_DIST;
}

void setup() {
  Serial.begin(9600);
  pinMode(TRIG_1, OUTPUT); pinMode(ECHO_1, INPUT);
  pinMode(TRIG_2, OUTPUT); pinMode(ECHO_2, INPUT);
  pinMode(AIN1, OUTPUT); pinMode(AIN2, OUTPUT); pinMode(PWMA, OUTPUT);
  actuatorRetract();
  delay(CLOSE_TIME_MS);
  actuatorStop();
  currentState = IDLE;
  Serial.println("SmartBin v0.2 ready.");
}

void loop() {
  if (Serial.available() > 0) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (cmd == "OPEN_BIN" && currentState == IDLE) {
      actuatorExtend();
      stateStartTime = millis();
      currentState = OPENING;
    }
  }

  switch (currentState) {
    case IDLE:
      if (isHandNear()) {
        actuatorExtend();
        stateStartTime = millis();
        currentState = OPENING;
      }
      break;

    case OPENING:
      if (millis() - stateStartTime >= OPEN_TIME_MS) {
        actuatorStop();
        stateStartTime = millis();
        currentState = OPEN;
      }
      break;

    case OPEN:
      if (isPersonNear()) {
        stateStartTime = millis();  // reset close timer while someone is present
      } else if (millis() - stateStartTime >= 2000) {
        actuatorRetract();
        stateStartTime = millis();
        currentState = CLOSING;
      }
      break;

    case CLOSING:
      if (millis() - stateStartTime >= CLOSE_TIME_MS) {
        actuatorStop();
        if (isBinFull()) {
          Serial.println("ALARM:FULL");
          currentState = FULL;
        } else {
          currentState = IDLE;
        }
      }
      break;

    case FULL:
      // Sensor-triggered open is blocked. Telegram command can still open.
      break;
  }
  delay(100);
}
