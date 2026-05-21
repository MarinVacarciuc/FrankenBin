# SmartBin — Automated Waste Bin

University IoT project (Unit 20). Proximity-activated smart bin with IoT remote control.

## Hardware (v0.3)
| Component | Purpose |
|---|---|
| Arduino Uno | Main controller (FSM logic, sensors, audio) |
| NodeMCU ESP8266 | WiFi + Telegram bot gateway |
| KT0905 linear actuator (30 mm, 5V) | Lid open/close |
| TB6612FNG motor driver | Drive linear actuator up to 1.2 A |
| HC-SR04 #1 (front) | Detect hand <20 cm → open lid |
| HC-SR04 #2 (inside lid) | Hold open while bin zone occupied; fill level |
| DFPlayer Mini | MP3 audio feedback |
| DHT11 | Temperature + humidity sensor |

## Architecture

Two-controller split introduced in this version:

```
[Arduino Uno] ←→ Serial 9600 baud ←→ [NodeMCU ESP8266] ←→ Telegram
     │
 All hardware
 (sensors, motors, audio, DHT11)
```

Arduino sends events: `LID:OPEN`, `LID:CLOSED`, `FILL:%`, `TEMP:`, `HUM:`, `ALARM:FULL`  
ESP sends single-char commands: `o` open, `c` close, `m` mute, `+`/`-` volume, `r` reset

## 5-State FSM
`IDLE → OPENING → OPEN → CLOSING → FULL_LOCKED`

FULL_LOCKED plays an "angry" audio track when someone approaches. Unlocked via Telegram `/UNLOCK`.

## Telegram STATUS output
```
Lid: CLOSED
Fill: 42%
Temperature: 22.5 C
Humidity: 58.0%
WiFi signal: -65 dBm
Lock: ACTIVE
```

## Wiring
| Signal | Arduino Pin |
|---|---|
| HC-SR04 #1 TRIG/ECHO | A0 / A1 |
| HC-SR04 #2 TRIG/ECHO | A2 / A3 |
| TB6612 AIN1, AIN2, PWMA | 8, 9, 5 |
| DFPlayer RX/TX | D10 / D11 (SoftwareSerial) |
| DFPlayer BUSY | D4 |
| DHT11 DATA | D7 |
| Tactile button | D12 |
| ESP8266 TX→RX / RX→TX | 0 / 1 (hardware Serial) |

> **Note:** disconnect pins 0/1 before uploading via USB.
