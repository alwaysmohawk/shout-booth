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
// =============================================================

#include <Arduino.h>

// ---- CONFIG -------------------------------------------------

// Print state transitions to USB serial; type 1/2/3 + Enter to simulate listener
#define DEBUG_MODE            false

// Red countdown flash (3 stages × STAGE_MS = total red intro)
#define RED_FLASH_HZ_1        10      // Hz stage 1
#define RED_FLASH_HZ_2        20      // Hz stage 2
#define RED_FLASH_HZ_3        40      // Hz stage 3
#define RED_FLASH_STAGE_MS    2000    // ms per stage

// Listening window
#define GREEN_LISTEN_MS       6000

// Set true for slow sine-wave ceiling pulse, false for solid
#define CEILING_GREEN_PULSE   true
#define GREEN_PULSE_HZ        0.75f   // cycles per second

// Post-listen hold
#define RED_HOLD_MS           4000

// Delay between sequential bank illuminations
#define SEQUENTIAL_DELAY_MS   500

// Illuminate animation (white banks turning on)
#define ILLUMINATE_RAMP_MS    300     // total fade-in duration
#define ILLUMINATE_FLASH_HZ   10     // strobe rate during ramp

// Fade-out (white banks, then ceiling, one by one)
#define FADE_DURATION_MS      1000   // per-bank

// Emergency stop (double-press)
#define EMERGENCY_FADE_MS     2000   // fade everything to off
#define DOUBLE_PRESS_MS       500    // detection window
#define DEBOUNCE_MS           50

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

// ---- STATE MACHINE ------------------------------------------
enum State { IDLE, RED_FLASH, GREEN_LISTEN, RED_HOLD, FADE_OUT, EMERGENCY_STOP };

static State     state      = IDLE;
static uint32_t  stateStart = 0;

// ---- BUTTON -------------------------------------------------
static bool     rawBtnLast      = HIGH;
static bool     debouncedBtn    = HIGH;
static uint32_t debounceTimer   = 0;
static uint32_t lastPressTime   = 0;
static int      pressCount      = 0;

// ---- LISTENER UART ------------------------------------------
HardwareSerial ListenerSerial(2);

// ---- LEVEL / BANK STATE -------------------------------------
static bool levelReached[4] = {};   // [1..3] set by listener UART

static int      litUpTo     = 0;    // highest bank currently illuminating
static uint32_t lastLitTime = 0;

struct BankAnim {
    bool     active;
    uint32_t startMs;
    bool     done;      // ramp complete → solid on
};
static BankAnim bankAnim[4] = {};   // [1..3]

// Current output level for each LEDC channel — tracked so emergency
// stop can fade from wherever we currently are.
static uint32_t outPwm[5] = {};

// Brightnesses frozen at the start of FADE_OUT / EMERGENCY_STOP
static uint32_t fadeFromPwm[5] = {};

// ---- FADE-OUT -----------------------------------------------
static int      fadingBank  = 3;    // counts down 3→2→1→0(ceiling)
static uint32_t fadeStart   = 0;

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

// Bank channel lookup [1..3]
static const int bankCh[] = { 0, CH_CHEER, CH_HOLLER, CH_SHOUT };

// ---- ILLUMINATE ANIMATION -----------------------------------
static void startIlluminate(int bank) {
    bankAnim[bank] = { true, millis(), false };
}

// Returns the PWM value the bank should be at right now.
static uint32_t illuminatePwm(int bank, uint32_t now) {
    BankAnim& a = bankAnim[bank];
    if (!a.active) return 0;

    uint32_t elapsed = now - a.startMs;
    if (elapsed >= ILLUMINATE_RAMP_MS) {
        a.done = true;
        return MAX_PWM;
    }

    float    envelope  = (float)elapsed / ILLUMINATE_RAMP_MS;
    uint32_t peak      = (uint32_t)(envelope * MAX_PWM);
    uint32_t periodMs  = 1000u / ILLUMINATE_FLASH_HZ;
    uint32_t phase     = elapsed % periodMs;
    return (phase < periodMs / 2) ? peak : 0;
}

// ---- SEQUENTIAL BANK LOGIC ----------------------------------
// Called from both GREEN_LISTEN and RED_HOLD so a late-detected
// threshold still sequences correctly into the hold phase.
static void updateSequentialBanks(uint32_t now) {
    int next = litUpTo + 1;
    if (next > 3 || !levelReached[next]) return;

    bool canLight = (litUpTo == 0) || ((now - lastLitTime) >= SEQUENTIAL_DELAY_MS);
    if (!canLight) return;

    startIlluminate(next);
    litUpTo     = next;
    lastLitTime = now;
}

static void driveWhiteBanks(uint32_t now) {
    for (int i = 1; i <= 3; i++) {
        uint32_t pwm = illuminatePwm(i, now);
        setPwm(bankCh[i], pwm);
    }
}

// ---- BUTTON HANDLING ----------------------------------------
static void handleButton(uint32_t now) {
    bool raw = digitalRead(PIN_BUTTON);

    // Debounce
    if (raw != rawBtnLast) debounceTimer = now;
    rawBtnLast = raw;

    if ((now - debounceTimer) < DEBOUNCE_MS) return;

    bool prevDebounced = debouncedBtn;
    debouncedBtn = raw;

    if (prevDebounced == HIGH && debouncedBtn == LOW) {
        // Falling edge → button pressed
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
                // Reset all per-run state
                memset(levelReached, 0, sizeof(levelReached));
                memset(bankAnim,     0, sizeof(bankAnim));
                litUpTo = 0;
                allOff();
                ListenerSerial.print('C');
                enterState(RED_FLASH);
            }
        }
    }
}

// ---- STATE UPDATES ------------------------------------------
static void updateRedFlash(uint32_t now) {
    uint32_t elapsed = now - stateStart;

    uint32_t hz;
    if      (elapsed < RED_FLASH_STAGE_MS * 1) hz = RED_FLASH_HZ_1;
    else if (elapsed < RED_FLASH_STAGE_MS * 2) hz = RED_FLASH_HZ_2;
    else if (elapsed < RED_FLASH_STAGE_MS * 3) hz = RED_FLASH_HZ_3;
    else {
        setPwm(CH_CEILING_RED, 0);
        ListenerSerial.print('S');
        enterState(GREEN_LISTEN);
        return;
    }

    uint32_t periodMs = 1000u / hz;
    uint32_t phase    = elapsed % periodMs;
    setPwm(CH_CEILING_RED,   (phase < periodMs / 2) ? MAX_PWM : 0);
    setPwm(CH_CEILING_GREEN, 0);
}

static void updateGreenListen(uint32_t now) {
    uint32_t elapsed = now - stateStart;

    // Consume incoming threshold events (real listener)
    while (ListenerSerial.available()) {
        char c = (char)ListenerSerial.read();
        if (c >= '1' && c <= '3') {
            levelReached[c - '0'] = true;
            if (DEBUG_MODE) Serial.printf("[LISTENER] threshold %c received\n", c);
        }
    }

    updateSequentialBanks(now);
    driveWhiteBanks(now);

    // Ceiling green
    if (CEILING_GREEN_PULSE) {
        float    phi   = 2.0f * PI * GREEN_PULSE_HZ * (elapsed / 1000.0f);
        uint32_t bright = (uint32_t)((sinf(phi) + 1.0f) * 0.5f * MAX_PWM);
        setPwm(CH_CEILING_GREEN, bright);
    } else {
        setPwm(CH_CEILING_GREEN, MAX_PWM);
    }
    setPwm(CH_CEILING_RED, 0);

    if (elapsed >= GREEN_LISTEN_MS) {
        ListenerSerial.print('R');
        setPwm(CH_CEILING_GREEN, 0);
        setPwm(CH_CEILING_RED,   MAX_PWM);
        enterState(RED_HOLD);
    }
}

static void updateRedHold(uint32_t now) {
    uint32_t elapsed = now - stateStart;

    // A threshold hit very late in green phase can still sequence here
    updateSequentialBanks(now);
    driveWhiteBanks(now);
    setPwm(CH_CEILING_RED,   MAX_PWM);
    setPwm(CH_CEILING_GREEN, 0);

    if (elapsed >= RED_HOLD_MS) {
        // Snapshot brightnesses for fade
        memcpy(fadeFromPwm, outPwm, sizeof(outPwm));

        // Find the highest lit white bank to start the fade from
        fadingBank = 3;
        while (fadingBank > 0 && fadeFromPwm[bankCh[fadingBank]] == 0) fadingBank--;

        fadeStart = now;
        enterState(FADE_OUT);
    }
}

static void updateFadeOut(uint32_t now) {
    uint32_t elapsed = now - fadeStart;
    float    t       = 1.0f - fminf((float)elapsed / FADE_DURATION_MS, 1.0f);

    if (fadingBank > 0) {
        // Fading a white bank
        setPwm(bankCh[fadingBank], (uint32_t)(t * fadeFromPwm[bankCh[fadingBank]]));

        if (elapsed >= FADE_DURATION_MS) {
            setPwm(bankCh[fadingBank], 0);
            fadingBank--;
            // Skip any banks that were never lit
            while (fadingBank > 0 && fadeFromPwm[bankCh[fadingBank]] == 0) fadingBank--;
            fadeStart = now;
        }
    } else {
        // All white banks done — fade ceiling red
        setPwm(CH_CEILING_RED, (uint32_t)(t * fadeFromPwm[CH_CEILING_RED]));

        if (elapsed >= FADE_DURATION_MS) {
            allOff();
            enterState(IDLE);
        }
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
    for (int ch = 0; ch < 5; ch++) {
        ledcWrite(ch, (uint32_t)(t * fadeFromPwm[ch]));
    }
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

    if (DEBUG_MODE) {
        Serial.println("Controller ready. DEBUG_MODE on.");
        Serial.println("Press button to start. Type 1/2/3 + Enter to simulate listener thresholds.");
        Serial.printf("[STATE] %s\n", stateName(state));
    }
}

void loop() {
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
