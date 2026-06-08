# FrankenBin — Smart IoT Bin

University IoT project (Unit 20). Proximity-activated smart waste bin with Telegram remote control.

## Architecture

```
[Arduino Uno] ←——— HW Serial pins 0/1 ———→ [NodeMCU ESP8266] ←→ Telegram
      │              (9600 baud, cross-wired)
  All hardware:
  sensors, actuator, audio (DFPlayer), climate (DHT11)
```

**Cross-wire rule:**  
Arduino TX (pin 1) → NodeMCU D6 (GPIO12, SoftSerial RX)  
Arduino RX (pin 0) ← NodeMCU D7 (GPIO13, SoftSerial TX)

> **Upload warning:** always disconnect pins 0/1 before flashing Arduino via USB.

## Hardware
| Component | Purpose |
|---|---|
| Arduino Uno | FSM controller |
| NodeMCU ESP8266 | WiFi + Telegram gateway |
| KT0905 linear actuator (30 mm, 5V) | Lid open/close |
| TB6612FNG motor driver | Motor driver up to 1.2 A |
| HC-SR04 #1 (front) | Hand detection (<20 cm) |
| HC-SR04 #2 (inside lid) | Fill level + hold-open zone |
| DFPlayer Mini | MP3 audio (welcome + alert tracks) |
| DHT11 | Temperature + humidity |
| Tactile button | Manual open override |

## 5-State FSM (Arduino)
| State | Description |
|---|---|
| IDLE | Polling front sensor for hand |
| OPENING | Extending actuator (2.5 s), plays welcome track |
| OPEN | Waits for zone to clear; **BUSY pin** prevents close while audio plays |
| CLOSING | Retracting actuator (1.9 s) |
| FULL_LOCKED | Plays alert audio; rejects opens until unlocked via Telegram |

**BUSY pin discovery:** DFPlayer takes ~250 ms to pull BUSY LOW after a play command. Without a post-trigger delay, the close timer started before BUSY asserted, causing the lid to close mid-track. Fix: 250 ms delay added between `playMp3Folder()` and entering the OPEN polling loop.

## Serial Protocol (Arduino → ESP)
| Message | Meaning |
|---|---|
| `LID:OPEN` | Lid has opened |
| `LID:CLOSED` | Lid has closed |
| `FILL:%` | Fill percentage (0–100) |
| `TEMP:XX.X` | Temperature °C |
| `HUM:XX.X` | Humidity % |
| `ALARM:FULL` | Bin full — triggers Telegram alert + lock |
| `VOL:%` | New volume (0–100%) |
| `MUTE:ON/OFF` | Mute state changed |

## Commands (ESP → Arduino)
`o` open · `m` mute · `+` vol up · `-` vol down · `r` reset

## Wiring
| Signal | Arduino Pin |
|---|---|
| HC-SR04 #1 TRIG/ECHO | A0 / A1 |
| HC-SR04 #2 TRIG/ECHO | A2 / A3 |
| TB6612 AIN1, AIN2, PWMA | 8, 9, 5 |
| DFPlayer RX/TX (SoftwareSerial) | D10 / D11 |
| DFPlayer BUSY | D4 |
| DHT11 DATA | D7 |
| Tactile button | D12 |
| M2M TX/RX (Hardware Serial) | 0 / 1 |
