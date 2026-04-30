# SmartBin — Automated Waste Bin

University IoT project (Unit 20). Proximity-activated bin lid controlled by Arduino Uno.

## Hardware (v0.1)
| Component | Purpose |
|---|---|
| Arduino Uno | Main controller |
| L293D motor driver | Drive DVD stepper motors |
| 2× DVD stepper motors | Open/close lid |
| HC-SR04 | Detect hand (<20 cm) |
| ESP8266 | Telegram remote control |

## How it works
1. HC-SR04 detects a hand within 20 cm.
2. Arduino drives L293D to step both motors forward (open).
3. After 3 seconds the lid closes automatically.
4. Telegram bot (via ESP8266) can open/close/lock remotely.

## Wiring
- L293D pins → Arduino 8, 9, 10, 11
- HC-SR04 TRIG → pin 12, ECHO → pin 13
- ESP8266 TX → Arduino RX (pin 0), ESP RX → Arduino TX (pin 1)
