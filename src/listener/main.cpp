// =============================================================
//  SHOUT BOOTH — LISTENER  (ESP32-C3 + INMP441)
// =============================================================
//
//  WIRING
//  ------
//  INMP441 VDD  → 3.3V
//  INMP441 GND  → GND
//  INMP441 L/R  → GND  (selects left channel)
//  INMP441 WS   → GPIO 4
//  INMP441 SCK  → GPIO 5
//  INMP441 SD   → GPIO 6
//
//  GPIO  7  : UART TX → controller RX (GPIO 16)
//  GPIO 10  : UART RX ← controller TX (GPIO 17)
//
//  UART PROTOCOL (115200 8N1)
//  --------------------------
//  RX from controller: 'S' = start listening, 'R' = reset
//  TX to controller:   '1' / '2' / '3' = threshold reached
//
//  THRESHOLD LOGIC
//  ---------------
//  Level 1: fixed dBSPL (FIXED_THRESHOLD_1_DB)
//  Level 2: measured dB at level-1 crossing + THRESHOLD_DELTA_DB
//  Level 3: measured dB at level-1 crossing + 2 × THRESHOLD_DELTA_DB
//
//  NOTE: dBSPL values here are approximate. FIXED_THRESHOLD_1_DB
//  may need trimming via the serial monitor (raw dB is printed
//  every second while listening) to suit your actual microphone
//  and enclosure acoustic environment.
// =============================================================

#include <Arduino.h>
#include "driver/i2s_std.h"

// ---- CONFIG -------------------------------------------------
#define FIXED_THRESHOLD_1_DB   65.0f   // dBSPL for level 1 (~loud conversation)
#define THRESHOLD_DELTA_DB      5.0f   // dB step between levels 2 and 3

// INMP441: -26 dBFS at 94 dBSPL → offset = 94 + 26 = 120
#define INMP441_OFFSET_DB     120.0f

#define SAMPLE_RATE            16000
#define SAMPLES_PER_BLOCK       512    // ~32 ms per read

// ---- PINS ---------------------------------------------------
#define I2S_WS_PIN   4
#define I2S_SCK_PIN  5
#define I2S_SD_PIN   6
#define UART_TX_PIN  7
#define UART_RX_PIN  10

// ---- GLOBALS ------------------------------------------------
static i2s_chan_handle_t rx_handle = NULL;
static int32_t           i2s_buf[SAMPLES_PER_BLOCK];

HardwareSerial ControllerSerial(1);

static bool  listening   = false;
static bool  level1Hit   = false;
static bool  level2Hit   = false;
static bool  level3Hit   = false;
static float threshold2  = 0.0f;
static float threshold3  = 0.0f;

// Throttle the debug print to once per second
static uint32_t lastPrintMs = 0;

// ---- I2S INIT -----------------------------------------------
static void i2s_init() {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &rx_handle));

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_32BIT,
                        I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)I2S_SCK_PIN,
            .ws   = (gpio_num_t)I2S_WS_PIN,
            .dout = I2S_GPIO_UNUSED,
            .din  = (gpio_num_t)I2S_SD_PIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_handle));
}

// ---- AUDIO MEASUREMENT --------------------------------------
// INMP441 outputs 24-bit audio left-justified inside a 32-bit slot.
// Shift right by 8 to sign-extend the 24-bit value, then normalise.
static float readDbSPL() {
    size_t bytes_read = 0;
    i2s_channel_read(rx_handle, i2s_buf, sizeof(i2s_buf), &bytes_read, pdMS_TO_TICKS(100));

    int n = (int)(bytes_read / sizeof(int32_t));
    if (n == 0) return 0.0f;

    double sum_sq = 0.0;
    for (int i = 0; i < n; i++) {
        float s = (float)(i2s_buf[i] >> 8) / 8388608.0f;   // [-1, 1]
        sum_sq += (double)s * s;
    }

    float rms  = sqrtf((float)(sum_sq / n));
    float dBFS = 20.0f * log10f(rms < 1e-9f ? 1e-9f : rms);
    return dBFS + INMP441_OFFSET_DB;
}

// ---- STATE RESET --------------------------------------------
static void resetState() {
    listening  = false;
    level1Hit  = level2Hit = level3Hit = false;
    threshold2 = threshold3 = 0.0f;
}

// ---- SETUP / LOOP -------------------------------------------
void setup() {
    Serial.begin(115200);
    ControllerSerial.begin(115200, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);
    i2s_init();
    Serial.println("Listener ready.");
}

void loop() {
    // Handle commands from controller
    while (ControllerSerial.available()) {
        char c = (char)ControllerSerial.read();
        if      (c == 'S') { resetState(); listening = true;  }
        else if (c == 'R') { resetState(); }
    }

    if (!listening) return;

    float db = readDbSPL();

    // Debug: print dB to serial once per second so you can calibrate
    // FIXED_THRESHOLD_1_DB for your booth's acoustic environment.
    uint32_t now = millis();
    if (now - lastPrintMs >= 1000) {
        Serial.printf("dBSPL: %.1f\n", db);
        lastPrintMs = now;
    }

    if (!level1Hit && db >= FIXED_THRESHOLD_1_DB) {
        level1Hit  = true;
        threshold2 = db + THRESHOLD_DELTA_DB;
        threshold3 = db + (2.0f * THRESHOLD_DELTA_DB);
        Serial.printf("Level 1 hit at %.1f dB  |  T2=%.1f  T3=%.1f\n",
                      db, threshold2, threshold3);
        ControllerSerial.print('1');
    }

    if (level1Hit && !level2Hit && db >= threshold2) {
        level2Hit = true;
        Serial.printf("Level 2 hit at %.1f dB\n", db);
        ControllerSerial.print('2');
    }

    if (level2Hit && !level3Hit && db >= threshold3) {
        level3Hit = true;
        Serial.printf("Level 3 hit at %.1f dB\n", db);
        ControllerSerial.print('3');
    }
}
