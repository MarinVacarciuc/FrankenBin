// SmartBin v0.1 — L293D + DVD stepper motors + HC-SR04 proximity trigger.
// Telegram commands (OPEN_BIN / CLOSE_BIN) arrive from ESP8266 via hardware Serial.

const int L293D_1 = 8;
const int L293D_2 = 9;
const int L293D_3 = 10;
const int L293D_4 = 11;

const int TRIG = 12;
const int ECHO = 13;

const int STEPS_PER_CYCLE = 20;
const int STEP_DELAY_MS   = 10;

void setup() {
  Serial.begin(115200);
  pinMode(L293D_1, OUTPUT); pinMode(L293D_2, OUTPUT);
  pinMode(L293D_3, OUTPUT); pinMode(L293D_4, OUTPUT);
  pinMode(TRIG, OUTPUT);    pinMode(ECHO, INPUT);
  Serial.println("SmartBin v0.1 ready.");
}

void stepMotor(int stepNum, bool reverse) {
  int phase = stepNum % 4;
  if (reverse) phase = 3 - phase;
  switch (phase) {
    case 0: digitalWrite(L293D_1,HIGH); digitalWrite(L293D_2,LOW);  digitalWrite(L293D_3,HIGH); digitalWrite(L293D_4,LOW);  break;
    case 1: digitalWrite(L293D_1,LOW);  digitalWrite(L293D_2,HIGH); digitalWrite(L293D_3,HIGH); digitalWrite(L293D_4,LOW);  break;
    case 2: digitalWrite(L293D_1,LOW);  digitalWrite(L293D_2,HIGH); digitalWrite(L293D_3,LOW);  digitalWrite(L293D_4,HIGH); break;
    case 3: digitalWrite(L293D_1,HIGH); digitalWrite(L293D_2,LOW);  digitalWrite(L293D_3,LOW);  digitalWrite(L293D_4,HIGH); break;
  }
  delay(STEP_DELAY_MS);
}

void motorOff() {
  digitalWrite(L293D_1,LOW); digitalWrite(L293D_2,LOW);
  digitalWrite(L293D_3,LOW); digitalWrite(L293D_4,LOW);
}

long readDistance() {
  digitalWrite(TRIG, LOW);  delayMicroseconds(2);
  digitalWrite(TRIG, HIGH); delayMicroseconds(10);
  digitalWrite(TRIG, LOW);
  long d = pulseIn(ECHO, HIGH, 30000);
  return d > 0 ? d / 58 : 999;
}

void openLid() {
  for (int i = 0; i < STEPS_PER_CYCLE * 10; i++) stepMotor(i, false);
  motorOff();
}

void closeLid() {
  for (int i = 0; i < STEPS_PER_CYCLE * 10; i++) stepMotor(i, true);
  motorOff();
}

void loop() {
  if (Serial.available() > 0) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (cmd == "OPEN_BIN")  { openLid(); delay(3000); closeLid(); }
    if (cmd == "CLOSE_BIN") { closeLid(); }
  }

  long dist = readDistance();
  if (dist > 0 && dist < 20) {
    Serial.println("MOTION");
    openLid();
    delay(3000);
    closeLid();
    delay(1000);
  }
  delay(200);
}
