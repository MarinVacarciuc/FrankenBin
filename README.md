# FrankenBin — Smart IoT Bin

University IoT project (Unit 20). Proximity-activated smart waste bin with Telegram control, ThingSpeak telemetry, and fire-alarm detection.

## Architecture

```
[Arduino Uno] ←—— HW Serial 0/1 (9600 baud) ——→ [NodeMCU ESP8266]
      │                 cross-wired                      │
  All hardware                                     Telegram bot
  (FSM, sensors,                                  ThingSpeak telemetry
   actuator, audio,                               OTA firmware updates
   DHT11, watchdog)
```

**Cross-wire:**  
Arduino TX (pin 1) → NodeMCU D6 (GPIO12, SoftSerial RX)  
Arduino RX (pin 0) ← NodeMCU D7 (GPIO13, SoftSerial TX)

> **Upload note:** disconnect pins 0/1 before flashing Arduino via USB.

## Hardware
| Component | Purpose |
|---|---|
| Arduino Uno | FSM + hardware control |
| NodeMCU ESP8266 | WiFi, Telegram, ThingSpeak, OTA |
| KT0905 linear actuator (30 mm, 5V) | Lid open/close |
| TB6612FNG motor driver | Up to 1.2 A |
| HC-SR04 #1 (front) | Hand detection <20 cm |
| HC-SR04 #2 (inside lid) | Fill level + hold-open zone + anti-pinch |
| DFPlayer Mini | MP3 audio feedback |
| DHT11 | Temperature + humidity |
| Tactile button | Manual open |

## 6-State FSM (Arduino)
| State | Description |
|---|---|
| IDLE | Polls front sensor |
| OPENING | Extends actuator (2.5 s), plays welcome track |
| OPEN | Zone-clear timer; BUSY pin prevents early close |
| CLOSING | Anti-pinch polling; reverses if object < 4 cm |
| FULL_LOCKED | Alert audio on approach; unlocked via Telegram |
| FIRE_ALARM | Forced open; repeats `ALARM:FIRE` every 10 s |

## Serial Protocol
**Arduino → ESP**
| Message | Meaning |
|---|---|
| `LID:OPEN` / `LID:CLOSED` | Lid state |
| `FILL:%` | Fill level 0–100 |
| `TEMP:XX.X` | Temperature °C |
| `HUM:XX.X` | Humidity % |
| `ALARM:FULL` | Bin full — auto-lock |
| `ALARM:FIRE` | Temp ≥ 35 °C |
| `SOUND:vol%:muted` | Volume + mute state (single atomic message) |
| `SYS:BOOT` / `SYS:CRASH` | Startup / watchdog recovery |

**ESP → Arduino** (single chars)  
`o` open · `m` mute · `+` vol up · `-` vol down · `rr` reset (double byte required)

## Safety features
- **Anti-pinch:** lid reverses during CLOSING if HC-SR04 #2 detects < 4 cm
- **Active anti-pinch:** runs on every boot before lid reaches home position
- **Watchdog:** 4 s timeout; EEPROM crash flag → `SYS:CRASH` on next boot
- **Reset guard:** Arduino requires two 'r' bytes within 1 s (prevents noise-triggered resets)

## ThingSpeak
Channel **3405117** — Fill %, Temperature, Humidity, RSSI — posted every 60 s.

## Telegram Keyboard
```
[  Open  ] [ Status  ]
[  Vol+  ] [  Vol-   ]
[ Mute(50%)] [ Lock  ]
[ Details ]
```

Details → inline buttons: About Creator · ThingSpeak Charts · Source Code

## Wiring
| Signal | Arduino Pin |
|---|---|
| HC-SR04 #1 TRIG / ECHO | A0 / A1 |
| HC-SR04 #2 TRIG / ECHO | A2 / A3 |
| TB6612 AIN1 / AIN2 / PWMA | 8, 9, 5 |
| DFPlayer RX / TX | D10 / D11 |
| DFPlayer BUSY | D4 |
| DHT11 DATA | D7 |
| Tactile button | D12 |
| M2M Serial TX / RX | 0 / 1 |
