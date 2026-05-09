# SmartBin — Automated Waste Bin

University IoT project (Unit 20). Proximity-activated smart bin with IoT remote control.

## Hardware (v0.2)
| Component | Purpose |
|---|---|
| Arduino Uno | Main controller (FSM logic) |
| KT0905 linear actuator (30 mm, 5V) | Lid open/close — replaces DVD steppers |
| TB6612FNG motor driver | Drive linear actuator up to 1.2 A |
| HC-SR04 #1 (front) | Detect hand <30 cm → open lid |
| HC-SR04 #2 (inside lid) | Hold open while person present; measure fill level |
| ESP8266 | Telegram remote control |

## Why we switched from DVD motors
DVD stepper motors did not generate enough torque to lift the metal lid.
The KT0905 linear actuator provides 6N force and has built-in limit protectors.

## 5-State FSM
`IDLE → OPENING → OPEN → CLOSING → (FULL if bin >90%)`

## Wiring
| Signal | Pin |
|---|---|
| HC-SR04 #1 TRIG/ECHO | A0 / A1 |
| HC-SR04 #2 TRIG/ECHO | A2 / A3 |
| TB6612 AIN1, AIN2, PWMA | 8, 9, 5 |
| ESP8266 TX→RX / RX→TX | 0 / 1 |
