// =============================================================
//  SHOUT BOOTH — LISTENER  (ESP32-C3 + INMP441)
// =============================================================
//
//  WIRING
//  ------
//  INMP441 VDD  → 3.3V
//  INMP441 GND  → GND
//  INMP441 L/R  → GND  (selects left channel)
//  INMP441 SD   → GPIO 4
//  INMP441 WS   → GPIO 5
//  INMP441 SCK  → GPIO 6
//
//  GPIO  7  : UART TX → controller RX (GPIO 16)
//  GPIO 10  : UART RX ← controller TX (GPIO 17)
//
//  UART PROTOCOL (115200 8N1)
//  --------------------------
//  RX from controller: 'C' = start noise floor calibration (red countdown)
//                      'S' = start listening (green phase)
//                      'R' = reset
//  TX to controller:   '1' / '2' / '3' = threshold reached
//
//  THRESHOLD LOGIC
//  ---------------
//  Level 1: noise_floor + NOISE_FLOOR_HEADROOM_DB  (set fresh each run)
//  Level 2: measured dB at level-1 crossing + THRESHOLD_DELTA_DB
//  Level 3: measured dB at level-1 crossing + 2 × THRESHOLD_DELTA_DB
//
//  Calibration happens during the red countdown ('C' → 'S').
//  FIXED_THRESHOLD_1_DB is used as a fallback only if calibration
//  has never run (e.g. first boot with no button press yet).
// =============================================================

#include <Arduino.h>
#include "driver/i2s.h"

// ---- CONFIG -------------------------------------------------
#define FIXED_THRESHOLD_1_DB     88.0f   // fallback if no calibration has run
#define NOISE_FLOOR_HEADROOM_DB  15.0f   // threshold1 = noise_floor + this
#define THRESHOLD_DELTA_DB       10.0f   // dB step between levels 2 and 3

// INMP441: -26 dBFS at 94 dBSPL → offset = 94 + 26 = 120
#define INMP441_OFFSET_DB       120.0f

#define SAMPLE_RATE              16000
#define SAMPLES_PER_BLOCK         512    // ~32 ms per read

// ---- PINS ---------------------------------------------------
#define I2S_WS_PIN   5
#define I2S_SCK_PIN  6
#define I2S_SD_PIN   4
#define UART_TX_PIN  7
#define UART_RX_PIN  10

// ---- GLOBALS ------------------------------------------------
static int32_t i2s_buf[SAMPLES_PER_BLOCK];

HardwareSerial ControllerSerial(1);

// Calibration state
static bool  calibrating      = false;
static float calibSum         = 0.0f;
static int   calibCount       = 0;
static float dynamicThresh1   = FIXED_THRESHOLD_1_DB;  // updated after each calibration

// Listening state
static bool  listening  = false;
static bool  level1Hit  = false;
static bool  level2Hit  = false;
static bool  level3Hit  = false;
static float threshold2 = 0.0f;
static float threshold3 = 0.0f;

static uint32_t lastPrintMs = 0;

// ---- I2S INIT -----------------------------------------------
static void i2s_init() {
    i2s_config_t cfg = {
        .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate          = SAMPLE_RATE,
        .bits_per_sample      = I2S_BITS_PER_SAMPLE_32BIT,
        .channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count        = 4,
        .dma_buf_len          = SAMPLES_PER_BLOCK,
        .use_apll             = false,
    };
    i2s_pin_config_t pins = {
        .mck_io_num   = I2S_PIN_NO_CHANGE,
        .bck_io_num   = I2S_SCK_PIN,
        .ws_io_num    = I2S_WS_PIN,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num  = I2S_SD_PIN,
    };
    ESP_ERROR_CHECK(i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL));
    ESP_ERROR_CHECK(i2s_set_pin(I2S_NUM_0, &pins));
}

// ---- AUDIO MEASUREMENT --------------------------------------
// INMP441 outputs 24-bit audio left-justified inside a 32-bit slot.
// Shift right by 8 to sign-extend the 24-bit value, then normalise.
static float readDbSPL() {
    size_t bytes_read = 0;
    i2s_read(I2S_NUM_0, i2s_buf, sizeof(i2s_buf), &bytes_read, pdMS_TO_TICKS(100));

    int n = (int)(bytes_read / sizeof(int32_t));
    if (n == 0) return 0.0f;

    double sum_sq = 0.0;
    for (int i = 0; i < n; i++) {
        float s = (float)(i2s_buf[i] >> 8) / 8388608.0f;
        sum_sq += (double)s * s;
    }

    float rms  = sqrtf((float)(sum_sq / n));
    float dBFS = 20.0f * log10f(rms < 1e-9f ? 1e-9f : rms);
    return dBFS + INMP441_OFFSET_DB;
}

// ---- STATE RESET --------------------------------------------
static void resetState() {
    listening  = false;
    calibrating = false;
    level1Hit  = level2Hit = level3Hit = false;
    threshold2 = threshold3 = 0.0f;
}

// ---- SETUP / LOOP -------------------------------------------
void setup() {
    Serial.begin(115200);
    ControllerSerial.begin(115200, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);
    i2s_init();
    Serial.printf("Listener ready. Fallback threshold: %.1f dBSPL\n", dynamicThresh1);
}

void loop() {
    while (ControllerSerial.available()) {
        char c = (char)ControllerSerial.read();

        if (c == 'C') {
            // Begin noise floor calibration (runs during red countdown)
            resetState();
            calibrating = true;
            calibSum    = 0.0f;
            calibCount  = 0;
            Serial.println("Calibrating noise floor...");

        } else if (c == 'S') {
            // Finalise calibration and start listening
            calibrating = false;
            if (calibCount > 0) {
                float noiseFloor  = calibSum / calibCount;
                dynamicThresh1    = noiseFloor + NOISE_FLOOR_HEADROOM_DB;
                Serial.printf("Noise floor: %.1f dB  →  Threshold 1: %.1f dB\n",
                              noiseFloor, dynamicThresh1);
            } else {
                Serial.printf("No calibration data — using fallback %.1f dB\n", dynamicThresh1);
            }
            listening = true;

        } else if (c == 'R') {
            resetState();
        }
    }

    float db = readDbSPL();

    if (calibrating) {
        calibSum += db;
        calibCount++;
        return;
    }

    if (!listening) return;

    uint32_t now = millis();
    if (now - lastPrintMs >= 1000) {
        Serial.printf("dBSPL: %.1f\n", db);
        lastPrintMs = now;
    }

    if (!level1Hit && db >= dynamicThresh1) {
        level1Hit  = true;
        threshold2 = db + THRESHOLD_DELTA_DB;
        threshold3 = db + (2.0f * THRESHOLD_DELTA_DB);
        Serial.printf("Level 1 at %.1f dB  |  T2=%.1f  T3=%.1f\n",
                      db, threshold2, threshold3);
        ControllerSerial.print('1');
    }

    if (level1Hit && !level2Hit && db >= threshold2) {
        level2Hit = true;
        Serial.printf("Level 2 at %.1f dB\n", db);
        ControllerSerial.print('2');
    }

    if (level2Hit && !level3Hit && db >= threshold3) {
        level3Hit = true;
        Serial.printf("Level 3 at %.1f dB\n", db);
        ControllerSerial.print('3');
    }
}
