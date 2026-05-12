#include <Arduino.h>
#include "driver/i2s.h"

#define SAMPLE_RATE      16000
#define SAMPLES          512
#define INMP441_OFFSET   120.0f   // -26 dBFS sensitivity + 94 dBSPL ref

#define PIN_SD   4
#define PIN_WS   5
#define PIN_SCK  6

static int32_t buf[SAMPLES];

void setup() {
    Serial.begin(115200);
    while (!Serial) delay(10);   // wait for USB CDC to connect

    i2s_config_t cfg = {
        .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate          = SAMPLE_RATE,
        .bits_per_sample      = I2S_BITS_PER_SAMPLE_32BIT,
        .channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count        = 4,
        .dma_buf_len          = SAMPLES,
        .use_apll             = false,
    };
    i2s_pin_config_t pins = {
        .mck_io_num   = I2S_PIN_NO_CHANGE,
        .bck_io_num   = PIN_SCK,
        .ws_io_num    = PIN_WS,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num  = PIN_SD,
    };
    i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL);
    i2s_set_pin(I2S_NUM_0, &pins);

    Serial.println("Volume tester ready.");
}

void loop() {
    size_t bytes_read = 0;
    i2s_read(I2S_NUM_0, buf, sizeof(buf), &bytes_read, pdMS_TO_TICKS(100));

    int n = bytes_read / sizeof(int32_t);
    if (n == 0) return;

    double sum_sq = 0.0;
    for (int i = 0; i < n; i++) {
        float s = (float)(buf[i] >> 8) / 8388608.0f;
        sum_sq += (double)s * s;
    }

    float rms  = sqrtf((float)(sum_sq / n));
    float dBFS = 20.0f * log10f(rms < 1e-9f ? 1e-9f : rms);
    Serial.printf("%.1f dBSPL\n", dBFS + INMP441_OFFSET);
}
