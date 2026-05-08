# Shout Booth

An interactive installation where participants cheer for their team inside a booth. The louder they yell, the more LED banks illuminate. A red countdown gets them ready, a green light signals go, and the result is held for the crowd to see before fading out for the next guest.

---

## Hardware

Two boards, one cable between them.

| Board | Role |
|---|---|
| ESP32 DevKit | Main controller — button input, all LED outputs |
| ESP32-C3 DevKitM-1 | Listener — INMP441 microphone, sends threshold events over UART |

### LED outputs (all via MOSFET trigger modules)

| Bank | Colour | Description |
|---|---|---|
| Ceiling | Red + Green | RGB strip (only R and G channels used) |
| Bank 1 | White | Cheer |
| Bank 2 | White | Holler |
| Bank 3 | White | Shout |

---

## Wiring

### Controller (ESP32)

| GPIO | Function |
|---|---|
| 4 | Momentary button → GND (internal pull-up enabled) |
| 16 | UART RX ← Listener TX |
| 17 | UART TX → Listener RX |
| 25 | Ceiling RED (MOSFET trigger) |
| 26 | Ceiling GREEN (MOSFET trigger) |
| 27 | Bank 1 — CHEER (MOSFET trigger) |
| 32 | Bank 2 — HOLLER (MOSFET trigger) |
| 33 | Bank 3 — SHOUT (MOSFET trigger) |

### Listener (ESP32-C3)

| GPIO | Function |
|---|---|
| 4 | INMP441 WS (LRCK) |
| 5 | INMP441 SCK (BCLK) |
| 6 | INMP441 SD (data) |
| 7 | UART TX → Controller RX (GPIO 16) |
| 10 | UART RX ← Controller TX (GPIO 17) |

### INMP441 power

| Pin | Connect to |
|---|---|
| VDD | 3.3 V |
| GND | GND |
| L/R | GND (selects left channel) |

### Inter-board cable

```
Controller GPIO 17 (TX) ──────► Listener  GPIO 10 (RX)
Controller GPIO 16 (RX) ◄────── Listener  GPIO  7 (TX)
Controller GND          ──────── Listener  GND
```

---

## Experience flow

```
[IDLE] ──── single press ────►

[RED FLASH]   Ceiling flashes red at increasing speed
               Stage 1: 2 s at 10 Hz
               Stage 2: 2 s at 20 Hz
               Stage 3: 2 s at 40 Hz

[GREEN LISTEN] Ceiling goes green (solid or pulsing)
               Listener is active — participant cheers
               Banks illuminate sequentially as thresholds are crossed
               Duration: 6 s

[RED HOLD]    Ceiling solid red, lit banks held at full brightness
               Duration: 12 s

[FADE OUT]    White banks fade one by one (Shout → Holler → Cheer)
               then ceiling fades out
               Returns to IDLE, ready for next guest

──── double press (any time during experience) ────►

[EMERGENCY STOP]  All outputs fade to off over 2 s → IDLE
```

---

## Volume threshold logic

The thresholds are **dynamic**, not fixed.

1. **Level 1** is a fixed dBSPL value (`FIXED_THRESHOLD_1_DB`). When the participant's volume crosses it, Bank 1 (Cheer) starts illuminating and the crossing level is recorded.
2. **Level 2** = recorded level + `THRESHOLD_DELTA_DB`. When reached, Bank 2 (Holler) illuminates.
3. **Level 3** = recorded level + `2 × THRESHOLD_DELTA_DB`. When reached, Bank 3 (Shout) illuminates.

This means the upper two thresholds are always set relative to how loud the participant was when they first crossed the bar — giving everyone a fair but achievable challenge.

### Sequential display

Even if a participant's peak volume crosses all three thresholds at once, the banks illuminate one at a time with a `SEQUENTIAL_DELAY_MS` gap between them. The delay starts when each bank's illuminate animation begins, so three banks triggered simultaneously fill the 6 s green window evenly (2 s → 2 s → 2 s at default settings).

---

## Illuminate animation

When a white bank turns on it does not snap to full brightness. Instead it strobes at `ILLUMINATE_FLASH_HZ` while the peak brightness of each flash ramps from 0 to full over `ILLUMINATE_RAMP_MS`. After the ramp completes the bank stays solid at full brightness.

---

## Configuration reference

### Controller — `src/controller/main.cpp`

| Define | Default | Description |
|---|---|---|
| `RED_FLASH_HZ_1` | `10` | Flash rate (Hz) for countdown stage 1 |
| `RED_FLASH_HZ_2` | `20` | Flash rate (Hz) for countdown stage 2 |
| `RED_FLASH_HZ_3` | `40` | Flash rate (Hz) for countdown stage 3 |
| `RED_FLASH_STAGE_MS` | `2000` | Duration of each countdown stage (ms) |
| `GREEN_LISTEN_MS` | `6000` | Listening window duration (ms) |
| `CEILING_GREEN_PULSE` | `true` | `true` = slow sine pulse, `false` = solid green |
| `GREEN_PULSE_HZ` | `0.75` | Pulse frequency when `CEILING_GREEN_PULSE` is true (Hz) |
| `RED_HOLD_MS` | `12000` | Solid red hold after listening (ms) |
| `SEQUENTIAL_DELAY_MS` | `2000` | Gap between sequential bank illuminations (ms) |
| `ILLUMINATE_RAMP_MS` | `500` | Illuminate fade-in duration per bank (ms) |
| `ILLUMINATE_FLASH_HZ` | `10` | Strobe rate during illuminate ramp (Hz) |
| `FADE_DURATION_MS` | `1000` | Per-bank fade-out duration (ms) |
| `EMERGENCY_FADE_MS` | `2000` | All-off fade duration on double-press (ms) |
| `DOUBLE_PRESS_MS` | `500` | Double-press detection window (ms) |
| `DEBOUNCE_MS` | `50` | Button debounce time (ms) |

### Listener — `src/listener/main.cpp`

| Define | Default | Description |
|---|---|---|
| `FIXED_THRESHOLD_1_DB` | `65.0` | dBSPL required to trigger Level 1 (~loud conversation) |
| `THRESHOLD_DELTA_DB` | `5.0` | dB above Level 1 crossing for each subsequent level |
| `INMP441_OFFSET_DB` | `120.0` | Mic sensitivity offset (94 dBSPL ref + 26 dBFS sensitivity) |
| `SAMPLE_RATE` | `16000` | I2S sample rate (Hz) |
| `SAMPLES_PER_BLOCK` | `512` | Samples per RMS window (~32 ms at 16 kHz) |

---

## Calibrating the microphone

The dBSPL values are approximate. The INMP441 sensitivity varies between units and the acoustic environment of the booth will affect readings significantly.

To calibrate:

1. Flash only the listener firmware.
2. Open its serial monitor at 115200 baud.
3. Send the character `S` (start listening) from the serial monitor.
4. The listener will print `dBSPL: XX.X` once per second.
5. Note the ambient level and the level produced by a typical cheer.
6. Set `FIXED_THRESHOLD_1_DB` to a value comfortably above ambient but reachable with a moderate cheer.
7. Reflash the listener.

When a threshold is crossed, the listener also prints which level was hit and what dynamic thresholds were calculated, e.g.:

```
dBSPL: 58.3
dBSPL: 59.1
Level 1 hit at 67.4 dB  |  T2=72.4  T3=77.4
Level 2 hit at 73.1 dB
Level 3 hit at 78.2 dB
```

---

## UART protocol

Single ASCII characters, no framing, 115200 8N1.

| Direction | Byte | Meaning |
|---|---|---|
| Controller → Listener | `S` | Start listening (sent when green phase begins) |
| Controller → Listener | `R` | Reset (sent on emergency stop or between guests) |
| Listener → Controller | `1` | Level 1 threshold crossed |
| Listener → Controller | `2` | Level 2 threshold crossed |
| Listener → Controller | `3` | Level 3 threshold crossed |

---

## Build & flash

This is a PlatformIO project with two environments.

```bash
# Build both
pio run

# Flash controller (connect ESP32, adjust port if needed)
pio run -e controller -t upload

# Flash listener (connect ESP32-C3)
pio run -e listener -t upload

# Open serial monitor for listener calibration
pio device monitor -e listener
```

If `ledcAttachChannel` fails to compile, pin the platform version in `platformio.ini`:

```ini
[env:controller]
platform = espressif32@6.9.0
```

---

## PWM notes

All LED outputs use the ESP32 LEDC peripheral at a **5 kHz carrier / 12-bit resolution**. The strobe effects (countdown flash, illuminate ramp) are driven by the main loop switching LEDC duty between `MAX_PWM` (4095) and `0` at the desired strobe rate — the LEDC carrier frequency itself never changes. This gives both hardware-smooth brightness fading *and* independently controllable strobe timing.
