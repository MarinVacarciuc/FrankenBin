# FrankenBin — Smart IoT Bin

University IoT project (Unit 20). Proximity-activated smart waste bin with Telegram control, ThingSpeak telemetry, fire detection, and over-the-air updates.

> **Setup:** copy `code/nodemcu_gateway/secrets.h.example` to `secrets.h` and fill in your WiFi, Telegram, OTA, and ThingSpeak credentials before flashing the NodeMCU.

## Architecture

```
[Arduino Uno] <--- HW Serial 0/1 (9600 baud) ---> [NodeMCU ESP8266]
      |                 cross-wired                       |
  All hardware                                      Telegram bot
  (FSM, sensors,                                   ThingSpeak telemetry
   actuator, audio,                                OTA updates
   DHT11, watchdog)                                heartbeat monitor
```

**Cross-wire:**
Arduino TX (pin 1) → NodeMCU D7 (GPIO13, SoftSerial RX)
Arduino RX (pin 0) ← NodeMCU D6 (GPIO12, SoftSerial TX)

> **Upload note:** disconnect pins 0/1 before flashing the Arduino via USB.

## Hardware
| Component | Purpose |
|---|---|
| Arduino Uno | FSM + all hardware control |
| NodeMCU ESP8266 | WiFi, Telegram, ThingSpeak, OTA, heartbeat |
| KT0905 linear actuator (30 mm, 5V) | Lid open/close |
| TB6612FNG motor driver | Actuator driver (up to 1.2 A) |
| HC-SR04 #1 (front) | Hand detection < 20 cm |
| HC-SR04 #2 (inside lid) | Fill level + hold-open zone + anti-pinch |
| DFPlayer Mini | MP3 audio feedback |
| DHT11 | Temperature + humidity (fire trigger ≥ 35 °C) |
| Tactile pedal | Manual open / fire reset |

## 6-State FSM (Arduino)
| State | Description |
|---|---|
| IDLE | Polls front sensor; background climate monitor |
| OPENING | Extends actuator (2.5 s), plays welcome track, reports trigger source |
| OPEN | Holds open while occupied; BUSY pin prevents early close |
| CLOSING | Anti-pinch polling; reverses to OPENING if object < 4 cm |
| FULL_LOCKED | Alert audio on approach; opens only via pedal/Telegram |
| FIRE_ALARM | Lid snaps shut to cut oxygen, siren at 100 %, `ALARM:FIRE`; 180 s cooldown after reset |

## Serial Protocol
**Arduino → ESP**
| Message | Meaning |
|---|---|
| `LID:OPEN:<src>` | Lid opened; src = T (Telegram) / H (hand) / P (pedal) |
| `LID:CLOSED` | Lid closed |
| `FILL:%` | Fill level 0–100 |
| `TEMP:XX.X` / `HUM:XX.X` | Climate readings |
| `VOL:%` | Volume 0–100 (also signals un-mute) |
| `ALARM:FULL` | Bin full — auto-lock |
| `ALARM:FIRE` | Temp ≥ 35 °C |
| `ALARM:VANDAL` | Inner sensor reads empty space — lid likely removed |
| `SYS:BOOT` | Arduino started — gateway resyncs lid/fire/full state |
| `SYS:COOLDOWN` | Fire cleared, 180 s cooldown started |

**ESP → Arduino** (single chars)
`o` open · `c` open during fire (clear) · `m` mute toggle · `+`/`-` volume (auto-unmute) · `r` reset

## Features
- **Anti-pinch:** lid reverses during CLOSING if HC-SR04 #2 detects < 4 cm; also runs on every boot
- **Fire safety:** DHT11 ≥ 35 °C → lid shuts, siren at full volume, `ALARM:FIRE`; 180 s cooldown
- **Watchdog:** 8 s timeout; EEPROM crash flag → silent recovery (boot jingle skipped)
- **State resync:** gateway clears stale lid/fire/full flags on `SYS:BOOT` (no "already open" after an Arduino reset)
- **Heartbeat:** gateway warns in Telegram if the Arduino stops responding (~20 s)
- **Auto-unmute:** adjusting volume from Telegram un-mutes automatically
- **Password-protected reset:** both reset buttons require the OTA password; the password message is deleted from the chat after entry
- **Session audit:** the bot logs which user opened the bin or changed the volume

## ThingSpeak
Channel **3405117** — Fill %, Temperature, Humidity, RSSI — posted every 16 s.

## Telegram Keyboard
```
[ Status   ] [ Open         ]
[ Mute(%)  ] [ Details      ]
[ Vol -    ] [ Vol +        ]
[ Reset ESP] [ Reset Arduino]
```

- **Details** → inline buttons: About Creator · ThingSpeak Charts · Source Code
- Hidden command: `/laws` — the Three Laws of FrankenBin; `/firetest` - Firetest imitation.

## Wiring
| Signal | Arduino Pin |
|---|---|
| HC-SR04 #1 TRIG / ECHO | A0 / A1 |
| HC-SR04 #2 TRIG / ECHO | A2 / A3 |
| TB6612 AIN1 / AIN2 / PWMA | 8, 9, 5 |
| DFPlayer RX / TX | 10 / 11 |
| DFPlayer BUSY | 4 |
| DHT11 DATA | 7 |
| Tactile pedal | 12 |
| M2M Serial RX / TX | 0 / 1 |

## OTA Updates
The NodeMCU gateway supports ArduinoOTA. After the first USB flash, select **FrankenBin_Gateway** as the network port in the Arduino IDE and upload using the OTA password.
