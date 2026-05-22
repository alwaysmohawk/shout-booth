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
| 4 | INMP441 SD (data) |
| 5 | INMP441 WS (LRCK) |
| 6 | INMP441 SCK (BCLK) |
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

[RED FLASH]   Ceiling counts down in red
               1.5 s solid → 3 blinks → 1 s solid → 2 blinks → 1 s solid → 1 blink → 1 s solid
               (blink pattern can be disabled via web config for a plain solid-red countdown)

[GREEN LISTEN] Ceiling goes green (solid or pulsing)
               Listener is active — participant cheers
               Banks illuminate sequentially as thresholds are crossed
               Duration: 6 s

[RED HOLD]    Ceiling solid red, lit banks held at full brightness
               Duration: 5 s (configurable)

[FADE OUT]    White banks fade one by one (Shout → Holler → Cheer)
               then ceiling fades out
               Returns to IDLE, ready for next guest

──── double press (any time during experience) ────►

[EMERGENCY STOP]  All outputs fade to off over 2 s → IDLE
```

---

## Volume threshold logic

The thresholds are **dynamic**, calibrated fresh for every participant.

During the red countdown the listener continuously measures the room's ambient noise and accumulates a mean dBSPL reading. When the green phase begins:

1. **Level 1** = `noise_floor + NOISE_FLOOR_HEADROOM_DB`. The participant must break comfortably above the room noise to trigger it. When crossed, the crossing level is recorded and Bank 1 (Cheer) starts illuminating.
2. **Level 2** = recorded level + `THRESHOLD_DELTA_DB`. When reached, Bank 2 (Holler) illuminates.
3. **Level 3** = recorded level + `2 × THRESHOLD_DELTA_DB`. When reached, Bank 3 (Shout) illuminates.

The upper two thresholds are always set relative to how loud the participant was when they first crossed the bar — giving everyone a fair but achievable challenge. If the button is pressed before calibration has run (first boot), `FIXED_THRESHOLD_1_DB` is used as the Level 1 fallback.

If the calculated steps would push Level 2 or 3 above the configured mic ceiling (`cfgMaxDbSpl`, default 115 dBSPL), the remaining headroom above the Level 1 crossing is divided proportionally across the two upper levels instead.

### Sequential display

Even if a participant's peak volume crosses all three thresholds at once, the banks illuminate one at a time with a `SEQUENTIAL_DELAY_MS` gap between them. The delay starts when each bank's illuminate animation begins, so three banks triggered simultaneously fill the 6 s green window evenly (2 s → 2 s → 2 s at default settings).

---

## Illuminate animation

When a white bank turns on it does not snap to full brightness. Instead it strobes at `ILLUMINATE_FLASH_HZ` while the peak brightness of each flash ramps from 0 to full over `ILLUMINATE_RAMP_MS`. After the ramp completes the bank stays solid at full brightness.

---

## Web interface

Both boards broadcast a WiFi access point for 60 seconds after boot.

| Board | SSID | Password | IP |
|---|---|---|---|
| Controller | `controller` | `wondermakr` | 192.168.4.1 |
| Listener | `listener` | `wondermakr` | 192.168.4.1 |

Browse to `192.168.4.1` to see the config page. Change values via the dropdowns and hit **Save & Reboot** — the board writes the new values to flash (NVS) and restarts. Connect again within 60 seconds to make further changes.

OTA firmware upload is available at `/update` on both boards.

The AP shuts itself down automatically once the 60-second window expires (or after the last connected client disconnects and the timer runs out). All experience functions work normally after the AP is off.

---

## Configuration reference

The values below are factory defaults. They can be changed at runtime via the web interface without reflashing.

### Controller — `src/controller/main.cpp`

**Runtime (web-configurable):**

| Setting | Default | Description |
|---|---|---|
| Countdown blinks | On | 3-2-1 blink pattern during the red countdown; disable for plain solid red |
| Ceiling green mode | Pulsing | Solid or slow sine-wave pulse during the green phase |
| Pulse frequency | 0.75 Hz | Speed of the sine-wave pulse |
| Listening window | 6 s | Duration of the green (cheering) phase |
| Sequential bank delay | 2 s | Gap between each LED bank illuminating |
| Illuminate ramp duration | 500 ms | Time for each bank to strobe-fade up to full |
| Illuminate strobe rate | 10 Hz | Flash frequency during the illuminate ramp |
| Result hold (red) | 5 s | How long the ceiling stays solid red after the cheer |
| Fade-out duration | 1 s | Per-bank fade-out time at the end of the experience |

**Compile-time only (edit `main.cpp` to change):**

| Define | Default | Description |
|---|---|---|
| `BLINK_OFF_MS` | `100` | Dark pulse width for each blink in the countdown (ms) |
| `BLINK_ON_MS` | `150` | On-gap between blinks within a group (ms) |
| `EMERGENCY_FADE_MS` | `2000` | All-off fade duration on double-press (ms) |
| `DOUBLE_PRESS_MS` | `500` | Double-press detection window (ms) |
| `DEBOUNCE_MS` | `50` | Button debounce time (ms) |

### Listener — `src/listener/main.cpp`

**Runtime (web-configurable):**

| Setting | Default | Description |
|---|---|---|
| Fallback Level 1 threshold | 88 dBSPL | Used before the first calibration has run |
| Noise floor headroom | 15 dB | dB above the calibrated noise floor that sets Level 1 |
| Threshold step | 10 dB | dB above Level 1 crossing that triggers Levels 2 and 3 |
| Mic ceiling | 115 dBSPL | Highest reliable level; Levels 2 & 3 are compressed proportionally if the normal step would exceed it |

**Compile-time only:**

| Define | Default | Description |
|---|---|---|
| `INMP441_OFFSET_DB` | `120.0` | Mic sensitivity offset (94 dBSPL ref + 26 dBFS sensitivity) |
| `SAMPLE_RATE` | `16000` | I2S sample rate (Hz) |
| `SAMPLES_PER_BLOCK` | `512` | Samples per RMS window (~32 ms at 16 kHz) |

---

## Calibrating the microphone

The dBSPL values are approximate. The INMP441 sensitivity varies between units and the acoustic environment of the booth will affect readings significantly.

To find suitable values for your installation:

1. Flash only the listener firmware.
2. Open its serial monitor at 115200 baud.
3. Send the character `S` (start listening) from the serial monitor.
4. The listener will print `dBSPL: XX.X` once per second.
5. Note the ambient level (noise floor) and the level produced by a typical cheer at the intended mic distance.
6. Use the web interface (connect to `listener` AP within 60 s of boot) to set:
   - **Fallback Level 1 threshold** — a value comfortably above your measured ambient level.
   - **Noise floor headroom** — how many dB of headroom above the auto-calibrated noise floor should be required to trigger Level 1 (default 15 dB works well for a booth).
   - **Threshold step** — the dB gap between levels (default 10 dB gives a reasonable spread across a typical shout range).
   - **Mic ceiling** — the highest dBSPL the microphone can reliably measure (default 115 dBSPL). Only needs changing if your unit measures noticeably higher or lower peaks.

During normal operation the noise floor is measured automatically during each red countdown, so the threshold adapts to the room without manual intervention.

When a threshold is crossed, the listener prints which level was hit and what dynamic thresholds were calculated:

```
Noise floor: 62.4 dB  →  Threshold 1: 77.4 dB
dBSPL: 65.1
dBSPL: 66.8
Level 1 at 79.2 dB  |  T2=89.2  T3=99.2
Level 2 at 90.1 dB
Level 3 at 100.3 dB
```

If the steps were compressed to fit below the mic ceiling, the Level 1 line will include `[compressed]`:

```
Level 1 at 108.3 dB  |  T2=110.5  T3=112.7 [compressed]
```

---

## UART protocol

Single ASCII characters, no framing, 115200 8N1.

| Direction | Byte | Meaning |
|---|---|---|
| Controller → Listener | `C` | Start noise floor calibration (sent at the start of the red countdown) |
| Controller → Listener | `S` | Finalise calibration and start listening (sent when green phase begins) |
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

Built binaries land at `.pio/build/controller/firmware.bin` and `.pio/build/listener/firmware.bin` and can be uploaded via the `/update` OTA page on each board.

---

## PWM notes

All LED outputs use the ESP32 LEDC peripheral at a **5 kHz carrier / 12-bit resolution**. The strobe effects (countdown flash, illuminate ramp) are driven by the main loop switching LEDC duty between `MAX_PWM` (4095) and `0` at the desired strobe rate — the LEDC carrier frequency itself never changes. This gives both hardware-smooth brightness fading *and* independently controllable strobe timing.
