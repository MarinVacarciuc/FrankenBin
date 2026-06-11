# FrankenBin — Smart IoT Bin

University IoT project (Unit 20). Proximity-activated smart waste bin with Telegram control, real-time telemetry, and fire-alarm detection.

## Architecture

```
[Arduino Uno] ←——— HW Serial 0/1 (9600 baud) ———→ [NodeMCU ESP8266]
      │                  cross-wired                       │
  All hardware                                       Telegram bot
  (FSM, sensors,                                    ThingSpeak telemetry
   actuator, audio,                                 OTA firmware updates
   DHT11, watchdog)
```

**Cross-wire:**  
Arduino TX (pin 1) → NodeMCU D6 (GPIO12, SoftSerial RX)  
Arduino RX (pin 0) ← NodeMCU D7 (GPIO13, SoftSerial TX)

## Hardware
| Component | Purpose |
|---|---|
| Arduino Uno | FSM + all hardware |
| NodeMCU ESP8266 | WiFi, Telegram, ThingSpeak, OTA |
| KT0905 linear actuator (30 mm, 5V) | Lid open/close |
| TB6612FNG motor driver | Up to 1.2 A |
| HC-SR04 #1 (front) | Hand detection (<20 cm) |
| HC-SR04 #2 (inside lid) | Fill level + hold-open + anti-pinch |
| DFPlayer Mini | MP3 audio feedback |
| DHT11 | Temperature + humidity |
| Tactile button | Manual open |

## 6-State FSM (Arduino)
| State | Description |
|---|---|
| IDLE | Polling front sensor |
| OPENING | Extend actuator (2.5 s) + play welcome track |
| OPEN | Zone-clear timer + BUSY pin guard |
| CLOSING | **Anti-pinch** polling; reverses if object < 4 cm |
| FULL_LOCKED | Plays alert audio on approach |
| **FIRE_ALARM** | Forced open; repeats ALARM:FIRE every 10 s |

**Anti-pinch** runs both in CLOSING (live polling) and during boot (setup).  
**Watchdog** resets Arduino after 4 s hang; EEPROM flag → Telegram crash alert.  
**Fire threshold:** 35 °C (demo value for sensor test).

## Serial Protocol (Arduino → ESP)
| Message | Meaning |
|---|---|
| `LID:OPEN` / `LID:CLOSED` | Lid state change |
| `FILL:%` | Fill level (0–100) |
| `TEMP:XX.X` | Temperature °C |
| `HUM:XX.X` | Humidity % |
| `ALARM:FULL` | Bin full |
| `ALARM:FIRE` | Fire detected |
| `VOL:%` | Volume changed |
| `MUTE:ON/OFF` | Mute state |
| `SYS:BOOT` | Normal startup |
| `SYS:CRASH` | Watchdog recovery |

## Commands (ESP → Arduino)
`o` open · `m` mute · `+` vol up · `-` vol down · `r` reset

## ThingSpeak
Channel **3405117** — fields: Fill %, Temperature, Humidity, RSSI. Posted every 60 s.

## Telegram Keyboard
```
[ Open   ]  [ Status  ]
[ Vol+   ]  [ Vol-    ]
[ Mute   ]  [ Lock    ]
[ Details ]
```

Details opens an inline keyboard with creator and chart links.

## Wiring
| Signal | Pin |
|---|---|
| HC-SR04 #1 TRIG/ECHO | A0 / A1 |
| HC-SR04 #2 TRIG/ECHO | A2 / A3 |
| TB6612 AIN1/AIN2/PWMA | 8, 9, 5 |
| DFPlayer RX/TX | D10 / D11 |
| DFPlayer BUSY | D4 |
| DHT11 | D7 |
| Tactile button | D12 |
| M2M Serial TX/RX | 0 / 1 |
