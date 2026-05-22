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
//  Level 1: noise_floor + cfgNfHeadroom  (set fresh each run)
//  Level 2: measured dB at level-1 crossing + cfgThreshDelta
//  Level 3: measured dB at level-1 crossing + 2 × cfgThreshDelta
//
//  Calibration happens during the red countdown ('C' → 'S').
//  cfgFixedT1 is used as a fallback only if calibration has
//  never run (e.g. first boot with no button press yet).
//
//  WEB INTERFACE
//  -------------
//  On boot the listener broadcasts a WiFi AP for AP_TIMEOUT_MS (60 s).
//  Connect to "listener" (password: wonder) and browse to 192.168.4.1.
//  Change config values via the dropdowns and hit Save — the board saves
//  to NVS and reboots with the new values. OTA firmware upload is at /update.
//  The AP shuts itself down automatically once the timeout expires.
//
// =============================================================

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <ElegantOTA.h>
#include "driver/i2s.h"

// ---- COMPILE-TIME CONFIG ------------------------------------
#define AP_SSID          "listener"
#define AP_PASS          "wondermakr"
#define AP_TIMEOUT_MS    60000

// INMP441: -26 dBFS at 94 dBSPL → offset = 94 + 26 = 120
#define INMP441_OFFSET_DB   120.0f

#define SAMPLE_RATE         16000
#define SAMPLES_PER_BLOCK   512    // ~32 ms per read

// ---- RUNTIME CONFIG DEFAULTS --------------------------------
#define DEFAULT_FIXED_T1       88.0f   // fallback Level 1 threshold (dBSPL)
#define DEFAULT_NF_HEADROOM    15.0f   // dB above noise floor → Level 1
#define DEFAULT_THRESH_DELTA   10.0f   // dB step between levels
#define DEFAULT_MAX_DB_SPL    114.0f   // mic ceiling; compresses thresholds if delta would exceed it

// ---- PINS ---------------------------------------------------
#define I2S_WS_PIN    5
#define I2S_SCK_PIN   6
#define I2S_SD_PIN    4
#define UART_TX_PIN   7
#define UART_RX_PIN   10

// ---- RUNTIME CONFIG VARS ------------------------------------
static float cfgFixedT1     = DEFAULT_FIXED_T1;
static float cfgNfHeadroom  = DEFAULT_NF_HEADROOM;
static float cfgThreshDelta = DEFAULT_THRESH_DELTA;
static float cfgMaxDbSpl    = DEFAULT_MAX_DB_SPL;

static Preferences prefs;

static void loadConfig() {
    prefs.begin("lstn", true);
    cfgFixedT1     = prefs.getFloat("fixed_t1",    DEFAULT_FIXED_T1);
    cfgNfHeadroom  = prefs.getFloat("nf_headroom", DEFAULT_NF_HEADROOM);
    cfgThreshDelta = prefs.getFloat("thresh_delta", DEFAULT_THRESH_DELTA);
    cfgMaxDbSpl    = prefs.getFloat("max_db_spl",  DEFAULT_MAX_DB_SPL);
    prefs.end();
}

static void saveConfig() {
    prefs.begin("lstn", false);
    prefs.putFloat("fixed_t1",    cfgFixedT1);
    prefs.putFloat("nf_headroom", cfgNfHeadroom);
    prefs.putFloat("thresh_delta", cfgThreshDelta);
    prefs.putFloat("max_db_spl",  cfgMaxDbSpl);
    prefs.end();
}

// ---- WEB INTERFACE ------------------------------------------
static WebServer  httpServer(80);
static DNSServer  dnsServer;
static bool       apActive       = false;
static uint32_t   lastActivityMs = 0;
static bool       pendingRestart = false;
static uint32_t   restartMs      = 0;

static String makeSelect(const char* name, String cur,
                         const char* const* vals, const char* const* labels, int n) {
    String s = "<select name='"; s += name; s += "'>";
    for (int i = 0; i < n; i++) {
        s += "<option value='"; s += vals[i]; s += "'";
        if (String(vals[i]) == cur) s += " selected";
        s += ">"; s += labels[i]; s += "</option>";
    }
    return s + "</select>";
}

static String buildPage() {
    char t1Str[8], hdStr[8], dtStr[8], maxStr[8];
    snprintf(t1Str,  sizeof(t1Str),  "%.1f", cfgFixedT1);
    snprintf(hdStr,  sizeof(hdStr),  "%.1f", cfgNfHeadroom);
    snprintf(dtStr,  sizeof(dtStr),  "%.1f", cfgThreshDelta);
    snprintf(maxStr, sizeof(maxStr), "%.1f", cfgMaxDbSpl);

    String h =
        "<!DOCTYPE html><html><head>"
        "<meta charset='UTF-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>Shout Booth \xe2\x80\x94 Listener</title>"
        "<style>"
        "body{font-family:sans-serif;max-width:500px;margin:32px auto;padding:0 16px;color:#222}"
        "h1{font-size:1.25em;margin-bottom:2px}"
        "h2{font-size:0.85em;color:#888;font-weight:normal;margin:0 0 24px}"
        ".lrow{display:flex;align-items:center;gap:6px;margin-top:20px}"
        ".lrow label{margin:0;flex:1;font-weight:600;font-size:0.9em}"
        ".info-btn{background:none;border:1px solid #ccc;border-radius:50%;width:20px;height:20px;"
        "font-size:11px;line-height:18px;cursor:pointer;color:#999;padding:0;flex-shrink:0}"
        ".info-btn:hover{border-color:#0d6efd;color:#0d6efd}"
        ".info-panel{display:none;font-size:0.8em;color:#555;background:#f5f5f5;"
        "border-left:3px solid #0d6efd;padding:8px 10px;margin:4px 0 6px;border-radius:0 3px 3px 0}"
        ".info-panel.open{display:block}"
        ".info-panel code{background:#fff;padding:1px 4px;border-radius:3px;color:#d63384;font-size:0.9em}"
        "select{width:100%;padding:7px;font-size:1em;border:1px solid #bbb;border-radius:4px;background:#fff}"
        "button[type=submit]{margin-top:28px;width:100%;padding:13px;background:#0d6efd;color:#fff;"
        "border:none;font-size:1em;cursor:pointer;border-radius:5px;font-weight:600}"
        "button[type=submit]:active{background:#0a58ca}"
        "hr{border:none;border-top:1px solid #eee;margin:28px 0 0}"
        ".ota{text-align:center;font-size:0.9em;padding:12px 0}"
        "a{color:#0d6efd;text-decoration:none}"
        "</style>"
        "<script>function i(b){b.parentElement.nextElementSibling.classList.toggle('open')}</script>"
        "</head><body>"
        "<h1>Shout Booth</h1>"
        "<h2>Listener &mdash; connected to " AP_SSID "</h2>"
        "<form method='POST' action='/save'>";

    #define INFO_BTN "<button type='button' class='info-btn' onclick='i(this)'>\xe2\x93\x98</button>"

    // ---- Starting volume trigger ----
    { const char* v[] = {"75.0","80.0","85.0","88.0","90.0","95.0","100.0"};
      const char* l[] = {"75 dBSPL \xe2\x80\x94 quiet conversation",
                         "80 dBSPL \xe2\x80\x94 loud conversation",
                         "85 dBSPL \xe2\x80\x94 raised voice",
                         "88 dBSPL (default)",
                         "90 dBSPL \xe2\x80\x94 shout",
                         "95 dBSPL \xe2\x80\x94 loud shout",
                         "100 dBSPL \xe2\x80\x94 very loud"};
      h += "<div class='lrow'><label>Starting volume trigger</label>" INFO_BTN "</div>"
           "<div class='info-panel'>The dBSPL level used to trigger Level 1 before the first "
           "auto-calibration has run (e.g. first boot). Check the serial monitor to find a good "
           "value for your installation. <code>cfgFixedT1</code></div>";
      h += makeSelect("fixed_t1", t1Str, v, l, 7); }

    // ---- Level 1 sensitivity ----
    { const char* v[] = {"8.0","10.0","12.0","15.0","18.0","20.0"};
      const char* l[] = {"8 dB \xe2\x80\x94 very sensitive",
                         "10 dB \xe2\x80\x94 sensitive",
                         "12 dB",
                         "15 dB (default)",
                         "18 dB \xe2\x80\x94 relaxed",
                         "20 dB \xe2\x80\x94 requires a proper shout"};
      h += "<div class='lrow'><label>Level 1 sensitivity</label>" INFO_BTN "</div>"
           "<div class='info-panel'>How many dB above the measured room noise the participant must "
           "reach to trigger Level 1. Calibration runs automatically during each red countdown. "
           "<code>cfgNfHeadroom</code></div>";
      h += makeSelect("nf_headroom", hdStr, v, l, 6); }

    // ---- Gap between levels ----
    { const char* v[] = {"5.0","7.0","8.0","10.0","12.0","15.0"};
      const char* l[] = {"5 dB \xe2\x80\x94 closely spaced",
                         "7 dB",
                         "8 dB",
                         "10 dB (default)",
                         "12 dB",
                         "15 dB \xe2\x80\x94 widely spaced"};
      h += "<div class='lrow'><label>Gap between levels</label>" INFO_BTN "</div>"
           "<div class='info-panel'>Preferred dB step above the Level 1 crossing for Level 2; "
           "Level 3 is twice this gap. If the steps would exceed the mic ceiling, the remaining "
           "range is split proportionally instead. <code>cfgThreshDelta</code></div>";
      h += makeSelect("thresh_delta", dtStr, v, l, 6); }

    // ---- Mic ceiling ----
    { const char* v[] = {"110.0","112.0","113.0","114.0","115.0","117.0"};
      const char* l[] = {"110 dBSPL",
                         "112 dBSPL",
                         "113 dBSPL",
                         "114 dBSPL (default)",
                         "115 dBSPL",
                         "117 dBSPL"};
      h += "<div class='lrow'><label>Mic ceiling</label>" INFO_BTN "</div>"
           "<div class='info-panel'>The highest dBSPL this microphone can reliably measure. "
           "Levels 2 and 3 are compressed to fit below this value if the normal gap would exceed it. "
           "<code>cfgMaxDbSpl</code></div>";
      h += makeSelect("max_db_spl", maxStr, v, l, 6); }

    #undef INFO_BTN

    h += "<button type='submit'>Save &amp; Reboot</button>"
         "</form>"
         "<hr><div class='ota'><a href='/update'>Firmware update (OTA)</a></div>"
         "</body></html>";
    return h;
}

static void setupWifi() {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASS);
    delay(100);

    dnsServer.start(53, "*", WiFi.softAPIP());

    httpServer.on("/", HTTP_GET, []() {
        lastActivityMs = millis();
        httpServer.send(200, "text/html", buildPage());
    });

    httpServer.on("/save", HTTP_POST, []() {
        lastActivityMs = millis();
        if (httpServer.hasArg("fixed_t1"))
            cfgFixedT1     = httpServer.arg("fixed_t1").toFloat();
        if (httpServer.hasArg("nf_headroom"))
            cfgNfHeadroom  = httpServer.arg("nf_headroom").toFloat();
        if (httpServer.hasArg("thresh_delta"))
            cfgThreshDelta = httpServer.arg("thresh_delta").toFloat();
        if (httpServer.hasArg("max_db_spl"))
            cfgMaxDbSpl    = httpServer.arg("max_db_spl").toFloat();
        saveConfig();
        httpServer.send(200, "text/html",
            "<!DOCTYPE html><html><head><meta charset='UTF-8'>"
            "<meta http-equiv='refresh' content='4;url=/'></head>"
            "<body style='font-family:sans-serif;text-align:center;padding-top:60px'>"
            "<h2>Saved. Rebooting&hellip;</h2>"
            "<p style='color:#888'>Reconnect to <b>" AP_SSID "</b> and the page will reload.</p>"
            "</body></html>");
        pendingRestart = true;
        restartMs      = millis() + 600;
    });

    httpServer.onNotFound([]() {
        lastActivityMs = millis();
        httpServer.sendHeader("Location", "/");
        httpServer.send(302, "text/plain", "");
    });

    ElegantOTA.begin(&httpServer);
    httpServer.begin();

    apActive       = true;
    lastActivityMs = millis();
}

static void loopWifi() {
    if (!apActive) return;

    if (WiFi.softAPgetStationNum() > 0) lastActivityMs = millis();

    if (millis() - lastActivityMs > AP_TIMEOUT_MS) {
        dnsServer.stop();
        httpServer.stop();
        WiFi.softAPdisconnect(true);
        WiFi.mode(WIFI_OFF);
        apActive = false;
        return;
    }

    dnsServer.processNextRequest();
    httpServer.handleClient();
    ElegantOTA.loop();

    if (pendingRestart && millis() > restartMs) ESP.restart();
}

// ---- GLOBALS ------------------------------------------------
static int32_t i2s_buf[SAMPLES_PER_BLOCK];

HardwareSerial ControllerSerial(1);

// Calibration state
static bool  calibrating    = false;
static float calibSum       = 0.0f;
static int   calibCount     = 0;
static float dynamicThresh1 = DEFAULT_FIXED_T1;  // updated after each calibration

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
    listening   = false;
    calibrating = false;
    level1Hit   = level2Hit = level3Hit = false;
    threshold2  = threshold3 = 0.0f;
}

// ---- SETUP / LOOP -------------------------------------------
void setup() {
    Serial.begin(115200);
    ControllerSerial.begin(115200, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);

    loadConfig();
    dynamicThresh1 = cfgFixedT1;  // start with the configured fallback

    setupWifi();
    i2s_init();

    Serial.printf("Listener ready. Fallback threshold: %.1f dBSPL\n", dynamicThresh1);
}

void loop() {
    loopWifi();

    while (ControllerSerial.available()) {
        char c = (char)ControllerSerial.read();

        if (c == 'C') {
            resetState();
            calibrating = true;
            calibSum    = 0.0f;
            calibCount  = 0;
            Serial.println("Calibrating noise floor...");

        } else if (c == 'S') {
            calibrating = false;
            if (calibCount > 0) {
                float noiseFloor = calibSum / calibCount;
                dynamicThresh1   = noiseFloor + cfgNfHeadroom;
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
        level1Hit = true;
        float step;
        if (db + 2.0f * cfgThreshDelta < cfgMaxDbSpl) {
            step = cfgThreshDelta;
        } else {
            // Fixed delta would exceed the mic ceiling — compress proportionally
            step = fmaxf((cfgMaxDbSpl - db) / 3.0f, 0.5f);
        }
        threshold2 = db + step;
        threshold3 = db + 2.0f * step;
        Serial.printf("Level 1 at %.1f dB  |  T2=%.1f  T3=%.1f%s\n",
                      db, threshold2, threshold3,
                      step < cfgThreshDelta ? " [compressed]" : "");
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
