// =============================================================
//  SHOUT BOOTH — MAIN CONTROLLER  (ESP32)
// =============================================================
//
//  WIRING
//  ------
//  GPIO  4  : Momentary button (to GND, internal pull-up used)
//  GPIO 16  : UART RX  ← listener TX  (Serial2 default)
//  GPIO 17  : UART TX  → listener RX  (Serial2 default)
//  GPIO 25  : Ceiling RED   (MOSFET trigger)
//  GPIO 26  : Ceiling GREEN (MOSFET trigger)
//  GPIO 27  : Bank 1 CHEER  (MOSFET trigger)
//  GPIO 32  : Bank 2 HOLLER (MOSFET trigger)
//  GPIO 33  : Bank 3 SHOUT  (MOSFET trigger)
//
//  UART PROTOCOL (115200 8N1)
//  --------------------------
//  Controller → Listener:  'C' = start noise floor calibration (red countdown)
//                          'S' = start listening (green phase)
//                          'R' = reset
//  Listener   → Controller: '1'/'2'/'3' = threshold reached
//
//  WEB INTERFACE
//  -------------
//  On boot the controller broadcasts a WiFi AP for AP_TIMEOUT_MS (60 s).
//  Connect to "controller" (password: wonder) and browse to 192.168.4.1.
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

// ---- COMPILE-TIME CONFIG ------------------------------------
// These are not exposed via the web interface.

// Print state transitions to USB serial; type 1/2/3 + Enter to simulate listener
#define DEBUG_MODE            false

// Red countdown blink animation (compile-time pulse widths)
#define BLINK_OFF_MS          100   // duration of each dark pulse
#define BLINK_ON_MS           150   // on-gap between blinks within a group

// Emergency stop (double-press)
#define EMERGENCY_FADE_MS     2000
#define DOUBLE_PRESS_MS       500
#define DEBOUNCE_MS           50

// WiFi AP
#define AP_SSID               "controller"
#define AP_PASS               "wondermakr"
#define AP_TIMEOUT_MS         60000   // shut down after 60 s of inactivity

// ---- RUNTIME CONFIG DEFAULTS --------------------------------
// These are the factory defaults. Once saved via the web interface the
// NVS values take over; flash the board and they revert to these.

#define DEFAULT_RED_BLINKS         true
#define DEFAULT_CEILING_PULSE      true
#define DEFAULT_GREEN_PULSE_HZ     0.75f
#define DEFAULT_GREEN_LISTEN_MS    6000
#define DEFAULT_SEQ_DELAY_MS       2000
#define DEFAULT_ILLU_RAMP_MS       500
#define DEFAULT_ILLU_FLASH_HZ      10
#define DEFAULT_RED_HOLD_MS        5000
#define DEFAULT_FADE_DUR_MS        1000

// ---- PINS / LEDC --------------------------------------------
#define PIN_BUTTON          4
#define PIN_CEILING_RED     25
#define PIN_CEILING_GREEN   26
#define PIN_CHEER           27
#define PIN_HOLLER          32
#define PIN_SHOUT           33
#define UART_RX_PIN         16
#define UART_TX_PIN         17

#define CH_CEILING_RED      0
#define CH_CEILING_GREEN    1
#define CH_CHEER            2
#define CH_HOLLER           3
#define CH_SHOUT            4

#define LEDC_FREQ           5000
#define LEDC_RES            12
#define MAX_PWM             4095

// ---- RUNTIME CONFIG VARS ------------------------------------
static bool     cfgRedBlinks     = DEFAULT_RED_BLINKS;
static bool     cfgCeilingPulse  = DEFAULT_CEILING_PULSE;
static float    cfgGreenPulseHz  = DEFAULT_GREEN_PULSE_HZ;
static uint32_t cfgGreenListenMs = DEFAULT_GREEN_LISTEN_MS;
static uint32_t cfgSeqDelayMs    = DEFAULT_SEQ_DELAY_MS;
static uint32_t cfgIlluRampMs    = DEFAULT_ILLU_RAMP_MS;
static uint32_t cfgIlluFlashHz   = DEFAULT_ILLU_FLASH_HZ;
static uint32_t cfgRedHoldMs     = DEFAULT_RED_HOLD_MS;
static uint32_t cfgFadeDurMs     = DEFAULT_FADE_DUR_MS;

static Preferences prefs;

static void loadConfig() {
    prefs.begin("ctrl", true);
    cfgRedBlinks     = prefs.getBool("red_blinks",      DEFAULT_RED_BLINKS);
    cfgCeilingPulse  = prefs.getBool("ceil_pulse",      DEFAULT_CEILING_PULSE);
    cfgGreenPulseHz  = prefs.getFloat("pulse_hz",       DEFAULT_GREEN_PULSE_HZ);
    cfgGreenListenMs = prefs.getUInt("grn_listen_ms",   DEFAULT_GREEN_LISTEN_MS);
    cfgSeqDelayMs    = prefs.getUInt("seq_delay_ms",    DEFAULT_SEQ_DELAY_MS);
    cfgIlluRampMs    = prefs.getUInt("illu_ramp_ms",    DEFAULT_ILLU_RAMP_MS);
    cfgIlluFlashHz   = prefs.getUInt("illu_flash_hz",   DEFAULT_ILLU_FLASH_HZ);
    cfgRedHoldMs     = prefs.getUInt("red_hold_ms",     DEFAULT_RED_HOLD_MS);
    cfgFadeDurMs     = prefs.getUInt("fade_dur_ms",     DEFAULT_FADE_DUR_MS);
    prefs.end();
}

static void saveConfig() {
    prefs.begin("ctrl", false);
    prefs.putBool("red_blinks",     cfgRedBlinks);
    prefs.putBool("ceil_pulse",     cfgCeilingPulse);
    prefs.putFloat("pulse_hz",      cfgGreenPulseHz);
    prefs.putUInt("grn_listen_ms",  cfgGreenListenMs);
    prefs.putUInt("seq_delay_ms",   cfgSeqDelayMs);
    prefs.putUInt("illu_ramp_ms",   cfgIlluRampMs);
    prefs.putUInt("illu_flash_hz",  cfgIlluFlashHz);
    prefs.putUInt("red_hold_ms",    cfgRedHoldMs);
    prefs.putUInt("fade_dur_ms",    cfgFadeDurMs);
    prefs.end();
}

// ---- WEB INTERFACE ------------------------------------------
static WebServer  httpServer(80);
static DNSServer  dnsServer;
static bool       apActive      = false;
static uint32_t   lastActivityMs = 0;
static bool       pendingRestart = false;
static uint32_t   restartMs      = 0;

// Builds a <select> element; cur is the currently-saved value as a string.
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
    // Pulse Hz needs 2-dp string for option matching
    char pulseHzStr[8];
    snprintf(pulseHzStr, sizeof(pulseHzStr), "%.2f", cfgGreenPulseHz);

    String h =
        "<!DOCTYPE html><html><head>"
        "<meta charset='UTF-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>Shout Booth \xe2\x80\x94 Controller</title>"
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
        "<h2>Controller &mdash; connected to " AP_SSID "</h2>"
        "<form method='POST' action='/save'>";

    #define INFO_BTN "<button type='button' class='info-btn' onclick='i(this)'>\xe2\x93\x98</button>"

    // ---- Countdown blinks ----
    { const char* v[] = {"1","0"};
      const char* l[] = {"On (default)","Off \xe2\x80\x94 solid red only"};
      h += "<div class='lrow'><label>Countdown blinks</label>" INFO_BTN "</div>"
           "<div class='info-panel'>Whether the red ceiling blinks off in a 3\xe2\x80\x932\xe2\x80\x931 pattern "
           "during the countdown. Disable for a plain solid-red countdown. "
           "<code>cfgRedBlinks</code></div>";
      h += makeSelect("red_blinks", cfgRedBlinks ? "1" : "0", v, l, 2); }

    // ---- Green ceiling style ----
    { const char* v[] = {"1","0"};
      const char* l[] = {"Pulsing (sine wave)","Solid"};
      h += "<div class='lrow'><label>Green ceiling style</label>" INFO_BTN "</div>"
           "<div class='info-panel'>Appearance of the green ceiling light during the cheering phase. "
           "<code>cfgCeilingPulse</code></div>";
      h += makeSelect("ceil_pulse", cfgCeilingPulse ? "1" : "0", v, l, 2); }

    // ---- Pulse speed ----
    { const char* v[] = {"0.25","0.50","0.75","1.00","1.50"};
      const char* l[] = {"0.25 Hz \xe2\x80\x94 very slow","0.50 Hz \xe2\x80\x94 slow",
                         "0.75 Hz \xe2\x80\x94 medium (default)","1.00 Hz \xe2\x80\x94 fast",
                         "1.50 Hz \xe2\x80\x94 rapid"};
      h += "<div class='lrow'><label>Pulse speed</label>" INFO_BTN "</div>"
           "<div class='info-panel'>Speed of the sine-wave pulse on the green ceiling. "
           "Only applies when pulsing mode is selected above. <code>cfgGreenPulseHz</code></div>";
      h += makeSelect("pulse_hz", pulseHzStr, v, l, 5); }

    // ---- Cheer time ----
    { const char* v[] = {"4000","5000","6000","8000","10000"};
      const char* l[] = {"4 s","5 s","6 s (default)","8 s","10 s"};
      h += "<div class='lrow'><label>Cheer time</label>" INFO_BTN "</div>"
           "<div class='info-panel'>Total duration of the green phase \xe2\x80\x94 how long the participant "
           "has to cheer. <code>cfgGreenListenMs</code></div>";
      h += makeSelect("green_listen_ms", String(cfgGreenListenMs), v, l, 5); }

    // ---- Delay between light banks ----
    { const char* v[] = {"1000","1500","2000","2500","3000"};
      const char* l[] = {"1 s","1.5 s","2 s (default)","2.5 s","3 s"};
      h += "<div class='lrow'><label>Delay between light banks</label>" INFO_BTN "</div>"
           "<div class='info-panel'>Minimum time between each LED bank illuminating after its threshold "
           "is crossed (Cheer \xe2\x86\x92 Holler \xe2\x86\x92 Shout). <code>cfgSeqDelayMs</code></div>";
      h += makeSelect("seq_delay_ms", String(cfgSeqDelayMs), v, l, 5); }

    // ---- Light turn-on speed ----
    { const char* v[] = {"250","500","750","1000"};
      const char* l[] = {"250 ms \xe2\x80\x94 snappy","500 ms (default)",
                         "750 ms \xe2\x80\x94 smooth","1000 ms \xe2\x80\x94 slow"};
      h += "<div class='lrow'><label>Light turn-on speed</label>" INFO_BTN "</div>"
           "<div class='info-panel'>How long each bank takes to strobe-fade up to full brightness "
           "when triggered. <code>cfgIlluRampMs</code></div>";
      h += makeSelect("illu_ramp_ms", String(cfgIlluRampMs), v, l, 4); }

    // ---- Strobe speed ----
    { const char* v[] = {"5","8","10","15","20"};
      const char* l[] = {"5 Hz \xe2\x80\x94 slow strobe","8 Hz","10 Hz (default)",
                         "15 Hz","20 Hz \xe2\x80\x94 fast strobe"};
      h += "<div class='lrow'><label>Strobe speed</label>" INFO_BTN "</div>"
           "<div class='info-panel'>Flash frequency of the strobe effect during the light turn-on ramp. "
           "<code>cfgIlluFlashHz</code></div>";
      h += makeSelect("illu_flash_hz", String(cfgIlluFlashHz), v, l, 5); }

    // ---- Result display time ----
    { const char* v[] = {"3000","4000","5000","7000","10000"};
      const char* l[] = {"3 s","4 s","5 s (default)","7 s","10 s"};
      h += "<div class='lrow'><label>Result display time</label>" INFO_BTN "</div>"
           "<div class='info-panel'>How long the ceiling stays solid red after the cheer, "
           "holding the result for the crowd to see. <code>cfgRedHoldMs</code></div>";
      h += makeSelect("red_hold_ms", String(cfgRedHoldMs), v, l, 5); }

    // ---- Fade-out speed ----
    { const char* v[] = {"500","750","1000","1500","2000"};
      const char* l[] = {"500 ms \xe2\x80\x94 quick","750 ms","1 s (default)",
                         "1.5 s","2 s \xe2\x80\x94 slow"};
      h += "<div class='lrow'><label>Fade-out speed</label>" INFO_BTN "</div>"
           "<div class='info-panel'>How long each bank takes to fade out at the end of the experience, "
           "one bank at a time. <code>cfgFadeDurMs</code></div>";
      h += makeSelect("fade_dur_ms", String(cfgFadeDurMs), v, l, 5); }

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
        if (httpServer.hasArg("red_blinks"))
            cfgRedBlinks     = httpServer.arg("red_blinks") == "1";
        if (httpServer.hasArg("ceil_pulse"))
            cfgCeilingPulse  = httpServer.arg("ceil_pulse") == "1";
        if (httpServer.hasArg("pulse_hz"))
            cfgGreenPulseHz  = httpServer.arg("pulse_hz").toFloat();
        if (httpServer.hasArg("green_listen_ms"))
            cfgGreenListenMs = httpServer.arg("green_listen_ms").toInt();
        if (httpServer.hasArg("seq_delay_ms"))
            cfgSeqDelayMs    = httpServer.arg("seq_delay_ms").toInt();
        if (httpServer.hasArg("illu_ramp_ms"))
            cfgIlluRampMs    = httpServer.arg("illu_ramp_ms").toInt();
        if (httpServer.hasArg("illu_flash_hz"))
            cfgIlluFlashHz   = httpServer.arg("illu_flash_hz").toInt();
        if (httpServer.hasArg("red_hold_ms"))
            cfgRedHoldMs     = httpServer.arg("red_hold_ms").toInt();
        if (httpServer.hasArg("fade_dur_ms"))
            cfgFadeDurMs     = httpServer.arg("fade_dur_ms").toInt();
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

    // Redirect anything else to the config page (captive portal)
    httpServer.onNotFound([]() {
        lastActivityMs = millis();
        httpServer.sendHeader("Location", "/");
        httpServer.send(302, "text/plain", "");
    });

    ElegantOTA.begin(&httpServer);
    httpServer.begin();

    apActive       = true;
    lastActivityMs = millis();

    if (DEBUG_MODE)
        Serial.printf("AP: %s  IP: %s\n", AP_SSID, WiFi.softAPIP().toString().c_str());
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
        if (DEBUG_MODE) Serial.println("AP timed out, WiFi off.");
        return;
    }

    dnsServer.processNextRequest();
    httpServer.handleClient();
    ElegantOTA.loop();

    if (pendingRestart && millis() > restartMs) ESP.restart();
}

// ---- STATE MACHINE ------------------------------------------
enum State { IDLE, RED_FLASH, GREEN_LISTEN, RED_HOLD, FADE_OUT, EMERGENCY_STOP };

static State    state      = IDLE;
static uint32_t stateStart = 0;

// ---- RED FLASH SEQUENCE -------------------------------------
// Each segment: how long (ms) and whether the ceiling red is on.
// Pattern: red → 1.5 s → 3 blinks → 1 s → 2 blinks → 1 s → 1 blink → 1 s → green
struct RedSeg { uint16_t ms; bool on; };

static const RedSeg kRedWithBlinks[] = {
    // 1.5 s initial red
    {1500, true},
    // 3 × (off → on)
    {BLINK_OFF_MS, false}, {BLINK_ON_MS, true},
    {BLINK_OFF_MS, false}, {BLINK_ON_MS, true},
    {BLINK_OFF_MS, false}, {BLINK_ON_MS, true},
    // 1 s solid
    {1000, true},
    // 2 × (off → on)
    {BLINK_OFF_MS, false}, {BLINK_ON_MS, true},
    {BLINK_OFF_MS, false}, {BLINK_ON_MS, true},
    // 1 s solid
    {1000, true},
    // 1 × off, then 1 s solid
    {BLINK_OFF_MS, false},
    {1000, true},
};

static const RedSeg kRedNoBlinks[] = {
    {1500, true},
    {1000, true},
    {1000, true},
    {1000, true},
};

static const RedSeg* redSeqPtr = kRedWithBlinks;
static int           redSeqLen = 0;
static int           redSegIdx = 0;
static uint32_t      redSegStart = 0;

// ---- BUTTON -------------------------------------------------
static bool     rawBtnLast    = HIGH;
static bool     debouncedBtn  = HIGH;
static uint32_t debounceTimer = 0;
static uint32_t lastPressTime = 0;
static int      pressCount    = 0;

// ---- LISTENER UART ------------------------------------------
HardwareSerial ListenerSerial(2);

// ---- LEVEL / BANK STATE -------------------------------------
static bool levelReached[4] = {};

static int      litUpTo     = 0;
static uint32_t lastLitTime = 0;

struct BankAnim { bool active; uint32_t startMs; bool done; };
static BankAnim bankAnim[4] = {};

static uint32_t outPwm[5]      = {};
static uint32_t fadeFromPwm[5] = {};

// ---- FADE-OUT -----------------------------------------------
static int      fadingBank = 3;
static uint32_t fadeStart  = 0;

// ---- HELPERS ------------------------------------------------
static void setPwm(int ch, uint32_t duty) {
    outPwm[ch] = duty;
    ledcWrite(ch, duty);
}

static void allOff() {
    for (int ch = 0; ch < 5; ch++) setPwm(ch, 0);
}

static const char* stateName(State s) {
    switch (s) {
        case IDLE:           return "IDLE";
        case RED_FLASH:      return "RED_FLASH";
        case GREEN_LISTEN:   return "GREEN_LISTEN";
        case RED_HOLD:       return "RED_HOLD";
        case FADE_OUT:       return "FADE_OUT";
        case EMERGENCY_STOP: return "EMERGENCY_STOP";
        default:             return "UNKNOWN";
    }
}

static void enterState(State s) {
    state      = s;
    stateStart = millis();
    if (DEBUG_MODE) Serial.printf("[STATE] %s\n", stateName(s));
}

static const int bankCh[] = { 0, CH_CHEER, CH_HOLLER, CH_SHOUT };

// ---- ILLUMINATE ANIMATION -----------------------------------
static void startIlluminate(int bank) {
    bankAnim[bank] = { true, millis(), false };
}

static uint32_t illuminatePwm(int bank, uint32_t now) {
    BankAnim& a = bankAnim[bank];
    if (!a.active) return 0;

    uint32_t elapsed = now - a.startMs;
    if (elapsed >= cfgIlluRampMs) { a.done = true; return MAX_PWM; }

    float    envelope = (float)elapsed / cfgIlluRampMs;
    uint32_t peak     = (uint32_t)(envelope * MAX_PWM);
    uint32_t periodMs = 1000u / cfgIlluFlashHz;
    uint32_t phase    = elapsed % periodMs;
    return (phase < periodMs / 2) ? peak : 0;
}

// ---- SEQUENTIAL BANK LOGIC ----------------------------------
static void updateSequentialBanks(uint32_t now) {
    int next = litUpTo + 1;
    if (next > 3 || !levelReached[next]) return;

    bool canLight = (litUpTo == 0) || ((now - lastLitTime) >= cfgSeqDelayMs);
    if (!canLight) return;

    startIlluminate(next);
    litUpTo     = next;
    lastLitTime = now;
}

static void driveWhiteBanks(uint32_t now) {
    for (int i = 1; i <= 3; i++) setPwm(bankCh[i], illuminatePwm(i, now));
}

// ---- BUTTON HANDLING ----------------------------------------
static void handleButton(uint32_t now) {
    bool raw = digitalRead(PIN_BUTTON);
    if (raw != rawBtnLast) debounceTimer = now;
    rawBtnLast = raw;
    if ((now - debounceTimer) < DEBOUNCE_MS) return;

    bool prev    = debouncedBtn;
    debouncedBtn = raw;

    if (prev == HIGH && debouncedBtn == LOW) {
        bool isDouble = (pressCount >= 1) && ((now - lastPressTime) < DOUBLE_PRESS_MS);
        pressCount    = isDouble ? 0 : 1;
        lastPressTime = now;

        if (isDouble) {
            if (state != IDLE && state != EMERGENCY_STOP) {
                memcpy(fadeFromPwm, outPwm, sizeof(outPwm));
                ListenerSerial.print('R');
                enterState(EMERGENCY_STOP);
            }
        } else {
            if (state == IDLE) {
                memset(levelReached, 0, sizeof(levelReached));
                memset(bankAnim,     0, sizeof(bankAnim));
                litUpTo = 0;
                allOff();
                ListenerSerial.print('C');
                enterState(RED_FLASH);
                // initialise blink sequence — ceiling goes red immediately
                if (cfgRedBlinks) {
                    redSeqPtr = kRedWithBlinks;
                    redSeqLen = (int)(sizeof(kRedWithBlinks) / sizeof(kRedWithBlinks[0]));
                } else {
                    redSeqPtr = kRedNoBlinks;
                    redSeqLen = (int)(sizeof(kRedNoBlinks) / sizeof(kRedNoBlinks[0]));
                }
                redSegIdx   = 0;
                redSegStart = now;
                setPwm(CH_CEILING_RED, redSeqPtr[0].on ? MAX_PWM : 0);
            }
        }
    }
}

// ---- STATE UPDATES ------------------------------------------
static void updateRedFlash(uint32_t now) {
    if (now - redSegStart >= redSeqPtr[redSegIdx].ms) {
        redSegStart += redSeqPtr[redSegIdx].ms;
        redSegIdx++;
        if (redSegIdx >= redSeqLen) {
            setPwm(CH_CEILING_RED, 0);
            ListenerSerial.print('S');
            enterState(GREEN_LISTEN);
            return;
        }
        setPwm(CH_CEILING_RED, redSeqPtr[redSegIdx].on ? MAX_PWM : 0);
    }
}

static void updateGreenListen(uint32_t now) {
    uint32_t elapsed = now - stateStart;

    while (ListenerSerial.available()) {
        char c = (char)ListenerSerial.read();
        if (c >= '1' && c <= '3') {
            levelReached[c - '0'] = true;
            if (DEBUG_MODE) Serial.printf("[LISTENER] threshold %c received\n", c);
        }
    }

    updateSequentialBanks(now);
    driveWhiteBanks(now);

    if (cfgCeilingPulse) {
        float    phi   = 2.0f * PI * cfgGreenPulseHz * (elapsed / 1000.0f);
        setPwm(CH_CEILING_GREEN, (uint32_t)((sinf(phi) + 1.0f) * 0.5f * MAX_PWM));
    } else {
        setPwm(CH_CEILING_GREEN, MAX_PWM);
    }
    setPwm(CH_CEILING_RED, 0);

    if (elapsed >= cfgGreenListenMs) {
        ListenerSerial.print('R');
        setPwm(CH_CEILING_GREEN, 0);
        setPwm(CH_CEILING_RED,   MAX_PWM);
        enterState(RED_HOLD);
    }
}

static void updateRedHold(uint32_t now) {
    updateSequentialBanks(now);
    driveWhiteBanks(now);
    setPwm(CH_CEILING_RED,   MAX_PWM);
    setPwm(CH_CEILING_GREEN, 0);

    if (now - stateStart >= cfgRedHoldMs) {
        memcpy(fadeFromPwm, outPwm, sizeof(outPwm));
        fadingBank = 3;
        while (fadingBank > 0 && fadeFromPwm[bankCh[fadingBank]] == 0) fadingBank--;
        fadeStart = now;
        enterState(FADE_OUT);
    }
}

static void updateFadeOut(uint32_t now) {
    uint32_t elapsed = now - fadeStart;
    float    t       = 1.0f - fminf((float)elapsed / cfgFadeDurMs, 1.0f);

    if (fadingBank > 0) {
        setPwm(bankCh[fadingBank], (uint32_t)(t * fadeFromPwm[bankCh[fadingBank]]));
        if (elapsed >= cfgFadeDurMs) {
            setPwm(bankCh[fadingBank], 0);
            fadingBank--;
            while (fadingBank > 0 && fadeFromPwm[bankCh[fadingBank]] == 0) fadingBank--;
            fadeStart = now;
        }
    } else {
        setPwm(CH_CEILING_RED, (uint32_t)(t * fadeFromPwm[CH_CEILING_RED]));
        if (elapsed >= cfgFadeDurMs) { allOff(); enterState(IDLE); }
    }
}

static void updateEmergencyStop(uint32_t now) {
    uint32_t elapsed = now - stateStart;
    if (elapsed >= EMERGENCY_FADE_MS) {
        allOff();
        memset(levelReached, 0, sizeof(levelReached));
        memset(bankAnim,     0, sizeof(bankAnim));
        litUpTo = 0;
        enterState(IDLE);
        return;
    }
    float t = 1.0f - (float)elapsed / EMERGENCY_FADE_MS;
    for (int ch = 0; ch < 5; ch++) ledcWrite(ch, (uint32_t)(t * fadeFromPwm[ch]));
}

// ---- DEBUG SERIAL -------------------------------------------
static void handleDebugSerial() {
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c >= '1' && c <= '3') {
            levelReached[c - '0'] = true;
            Serial.printf("[DEBUG] Injected threshold level %c\n", c);
        }
    }
}

// ---- SETUP / LOOP -------------------------------------------
void setup() {
    Serial.begin(115200);
    ListenerSerial.begin(115200, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);

    pinMode(PIN_BUTTON, INPUT_PULLUP);

    ledcSetup(CH_CEILING_RED,   LEDC_FREQ, LEDC_RES);
    ledcSetup(CH_CEILING_GREEN, LEDC_FREQ, LEDC_RES);
    ledcSetup(CH_CHEER,         LEDC_FREQ, LEDC_RES);
    ledcSetup(CH_HOLLER,        LEDC_FREQ, LEDC_RES);
    ledcSetup(CH_SHOUT,         LEDC_FREQ, LEDC_RES);
    ledcAttachPin(PIN_CEILING_RED,   CH_CEILING_RED);
    ledcAttachPin(PIN_CEILING_GREEN, CH_CEILING_GREEN);
    ledcAttachPin(PIN_CHEER,         CH_CHEER);
    ledcAttachPin(PIN_HOLLER,        CH_HOLLER);
    ledcAttachPin(PIN_SHOUT,         CH_SHOUT);
    allOff();

    loadConfig();
    setupWifi();

    if (DEBUG_MODE) {
        Serial.println("Controller ready. DEBUG_MODE on.");
        Serial.println("Type 1/2/3 + Enter to simulate listener thresholds.");
        Serial.printf("[STATE] %s\n", stateName(state));
    }
}

void loop() {
    loopWifi();
    uint32_t now = millis();
    handleButton(now);
    if (DEBUG_MODE) handleDebugSerial();

    switch (state) {
        case IDLE:           break;
        case RED_FLASH:      updateRedFlash(now);      break;
        case GREEN_LISTEN:   updateGreenListen(now);   break;
        case RED_HOLD:       updateRedHold(now);       break;
        case FADE_OUT:       updateFadeOut(now);       break;
        case EMERGENCY_STOP: updateEmergencyStop(now); break;
    }
}
