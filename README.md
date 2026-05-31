# FrankenBin — Smart IoT Bin

University IoT project (Unit 20). Proximity-activated smart waste bin with Telegram remote control.

## Architecture

```
[Arduino Uno] ←→ SoftwareSerial 9600 baud ←→ [NodeMCU ESP8266] ←→ Telegram
      │
  All hardware:
  sensors, actuator, audio (DFPlayer), climate (DHT11)
```

The project now uses two microcontrollers:
- **Arduino Uno** — all hardware logic (FSM, sensors, motor, audio)
- **NodeMCU ESP8266** — all networking (WiFi, Telegram bot)

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
| OPENING | Extending actuator (2.5 s) |
| OPEN | Waiting for zone to clear; BUSY pin prevents early close |
| CLOSING | Retracting actuator (1.9 s) |
| FULL_LOCKED | Plays alert audio; rejects opens until unlocked via Telegram |

## Serial Protocol (Arduino → ESP)
| Message | Meaning |
|---|---|
| `LID:OPEN` | Lid has opened |
| `LID:CLOSED` | Lid has closed |
| `FILL:%` | Fill percentage (0–100) |
| `TEMP:XX.X` | Temperature reading |
| `HUM:XX.X` | Humidity reading |
| `ALARM:FULL` | Bin full — triggers lock |
| `VOL:%` | New volume (0–100) |
| `MUTE:ON/OFF` | Mute state changed |

## Commands (ESP → Arduino)
`o` open · `m` mute · `+` vol up · `-` vol down · `r` reset

## Telegram Keyboard
```
[ Open  ]  [ Status ]
[ Vol+  ]  [ Vol-   ]
[ Mute  ]  [ Lock   ]
```

## Wiring
| Signal | Pin |
|---|---|
| HC-SR04 #1 TRIG/ECHO | A0 / A1 |
| HC-SR04 #2 TRIG/ECHO | A2 / A3 |
| TB6612 AIN1, AIN2, PWMA | 8, 9, 5 |
| DFPlayer RX/TX | D10 / D11 |
| DFPlayer BUSY | D4 |
| DHT11 | D7 |
| Tactile button | D12 |
| ESP8266 (SoftwareSerial) | D6 / D7 |
