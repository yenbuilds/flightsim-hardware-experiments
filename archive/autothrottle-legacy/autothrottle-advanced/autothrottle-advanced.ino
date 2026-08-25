// ======================================================
//  Teensy 4.1 Motorized Throttle (TB6612FNG)
//  Serial A/T + Manual + Mode Echo + Weighted Motion + Joystick Axis
//  + Virtual Detent & Reverse Zone on Lower 50% Travel
// ======================================================
// Wiring summary (single motor channel A):
//   8  -> AIN1
//   9  -> AIN2
//   10 -> PWMA   (PWM output ~25kHz)
//   7  -> STBY   (HIGH to enable; can tie to 5V or control via pin)
//   A0 -> Pot wiper (3.3V max to Teensy)
//   Pot outer legs -> 3.3V / GND
//   VM -> motor power (5–9V typical)
//   VCC -> 5V (logic)
//   GND -> common (Teensy + driver + power supply)
// ======================================================

#include <Arduino.h>

// ---------------- Debug Toggles ----------------
#define DEBUG_MODE   true     // General logging (pos/target/vel)
#define DEBUG_PID    false    // Detailed PID term logging
#define DEBUG_SERIAL true     // Serial command updates
#define DEBUG_REVERSE true    // Log when reverse zone toggles

// ---------------- Pins ----------------
const int potPin  = A0; // this is pin 14 on Teensy 4.1
const int in1Pin  = 8;
const int in2Pin  = 9;
const int pwmPin  = 10;
const int stbyPin = 7;
const int modePin = 2;     // HIGH = A/T active

// ---------------- Tuning / Limits ----------------
bool  invertPot   = false;
const int potMin  = 10;
const int potMax  = 1013;

// Virtual detent: split travel 50/50
const float detentFrac   = 0.5f; // 0.5 = middle of travel
const int   detentCounts = potMin + (int)((potMax - potMin) * detentFrac);

// Slightly more smoothing for heavy feel
const float alphaPos = 0.12;

// Tolerance so lever can settle quietly
const int   deadband = 4;
const int   holdBand = 10;

// PWM limits and ramp (asymmetric: gentle accel, stronger braking)
const int   minPWM          = 35;
const int   maxPWM          = 200;
const int   pwmRampUpStep   = 4;   // smaller = heavier, smoother acceleration
const int   pwmRampDownStep = 14;  // larger = stronger braking near target

// PID gains tuned for smoother motion with stronger braking
const float Kp = 0.7;
const float Ki = 0.015;
const float Kd = 3.5;       // stronger damping near target
const float iClamp   = 120.0;
const float vThresh  = 0.6;

const uint32_t serialTimeoutMs = 200;
uint32_t lastSerialMs = 0;

// ---------------- State ----------------
int   targetCounts;
float posFilt = 0, lastPos = 0, vel = 0, iTerm = 0;
bool  lastMode = false;
int   lastPWM = 0;

// Reverse-zone state (for logging / future logic)
bool  lastReverseActive = false;

// ======================================================
// ---------- TB6612FNG low-level control ---------------
// ======================================================
static inline void enableDriver(bool on) {
  digitalWrite(stbyPin, on ? HIGH : LOW);
}

static inline void driveForward(int pwm) {
  enableDriver(true);
  digitalWrite(in1Pin, HIGH);
  digitalWrite(in2Pin, LOW);
  analogWrite(pwmPin, pwm);
}

static inline void driveReverse(int pwm) {
  enableDriver(true);
  digitalWrite(in1Pin, LOW);
  digitalWrite(in2Pin, HIGH);
  analogWrite(pwmPin, pwm);
}

static inline void coast() {
  analogWrite(pwmPin, 0);
  digitalWrite(in1Pin, LOW);
  digitalWrite(in2Pin, LOW);
  enableDriver(true);
}

static inline void brake() {
  analogWrite(pwmPin, 0);
  digitalWrite(in1Pin, HIGH);
  digitalWrite(in2Pin, HIGH);
  enableDriver(true);
}

// ======================================================
// ---------------- Helper functions --------------------
// ======================================================
int readPot() {
  int r = analogRead(potPin);
  if (invertPot) r = 1023 - r;
  r = constrain(r, potMin, potMax);
  return r;
}

void softHold() {
  float e = (float)targetCounts - posFilt;

  if (fabs(vel) < vThresh && fabs(e) <= holdBand) {
    // Close enough and moving slowly → just coast
    coast();
  } else {
    // Gentle constant push toward target
    int tiny = minPWM;
    if (e > 0) driveForward(tiny);
    else       driveReverse(tiny);
  }
}

// ---------------- Throttle / Reverse mapping ----------
// Map physical position (counts) into:
// - Forward throttle axis (0–1023) using ONLY top 50% of travel
// - Reverse amount 0.0–1.0 in lower 50% of travel
// - reverseActive flag for "below detent" zone
int mapThrottleAndReverse(float posCounts, bool &reverseActive, float &reverseAmount) {
  // Clamp for safety
  float p = posCounts;
  if (p < potMin) p = potMin;
  if (p > potMax) p = potMax;

  if (p < detentCounts) {
    // ---- Reverse zone (below virtual detent) ----
    reverseActive = true;

    // 0.0 at detent, 1.0 at full aft
    reverseAmount = (detentCounts - p) / (float)(detentCounts - potMin);
    if (reverseAmount < 0.0f) reverseAmount = 0.0f;
    if (reverseAmount > 1.0f) reverseAmount = 1.0f;

    // Forward throttle axis stays at idle when in reverse zone
    return 0;
  } else {
    // ---- Forward throttle (top 50% physical travel) ----
    reverseActive = false;
    reverseAmount = 0.0f;

    // Map [detentCounts .. potMax] to [0 .. 1023]
    int val = map((int)p, detentCounts, potMax, 0, 1023);
    if (val < 0) val = 0;
    if (val > 1023) val = 1023;
    return val;
  }
}

// ======================================================
// ---------------- Serial command handler --------------
// ======================================================
void readSerialTarget() {
  while (Serial.available()) {
    static char buf[12];
    static int idx = 0;
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      buf[idx] = 0;
      if (buf[0] == 'T') {
        int v = atoi(buf + 1);
        v = constrain(v, potMin, potMax);
        targetCounts = v;
        lastSerialMs = millis();
        if (DEBUG_SERIAL) {
          Serial.print("[CMD] New target: ");
          Serial.println(targetCounts);
        }
      }
      idx = 0;
    } else if (idx < (int)sizeof(buf) - 1) {
      buf[idx++] = c;
    } else idx = 0;
  }
}

// ======================================================
// ---------------- PID control loop --------------------
// ======================================================
void runControlToTarget() {
  float e = (float)targetCounts - posFilt;

  // Inside tight deadband → don't bother with full PID, just soft-hold
  if (fabs(e) <= deadband) {
    iTerm *= 0.98f;   // bleed off integral slowly
    softHold();
    if (DEBUG_PID) {
      Serial.print("[PID] Hold e=");
      Serial.println(e, 2);
    }
    return;
  }

  // --- Full PID ---
  float pTerm = Kp * e;
  iTerm += Ki * e;
  iTerm = constrain(iTerm, -iClamp, iClamp);
  float dTerm = -Kd * vel;
  float u = pTerm + iTerm + dTerm;

  int pwm = (int)fabs(u);
  pwm = constrain(pwm, 0, maxPWM);
  if (pwm > 0 && pwm < minPWM) pwm = minPWM;

  // --- Distance-based speed cap near target (smoother arrival) ---
  float eAbs = fabs(e);
  const float slowDownDist = 120.0f; // counts: within this, start capping max speed
  if (eAbs < slowDownDist) {
    int dynamicMax = minPWM + (int)((maxPWM - minPWM) * (eAbs / slowDownDist));
    if (dynamicMax < minPWM) dynamicMax = minPWM;
    pwm = min(pwm, dynamicMax);
  }

  // --- Asymmetric ramp: gentle accel, stronger braking ---
  if (pwm > lastPWM) {
    int delta = pwm - lastPWM;
    if (delta > pwmRampUpStep) {
      pwm = lastPWM + pwmRampUpStep;   // accelerate slowly
    }
  } else {
    int delta = lastPWM - pwm;
    if (delta > pwmRampDownStep) {
      pwm = lastPWM - pwmRampDownStep; // brake harder
    }
  }
  lastPWM = pwm;

  if (DEBUG_PID) {
    Serial.print("[PID] e=");
    Serial.print(e, 1);
    Serial.print(" vel=");
    Serial.print(vel, 3);
    Serial.print(" P=");
    Serial.print(pTerm, 2);
    Serial.print(" I=");
    Serial.print(iTerm, 2);
    Serial.print(" D=");
    Serial.print(dTerm, 2);
    Serial.print(" u=");
    Serial.print(u, 2);
    Serial.print(" pwm=");
    Serial.println(pwm);
  }

  if (u > 0)      driveForward(pwm);
  else if (u < 0) driveReverse(pwm);
  else            coast();
}

// ======================================================
// ---------------- Setup & main loop -------------------
// ======================================================
void setup() {
  pinMode(in1Pin, OUTPUT);
  pinMode(in2Pin, OUTPUT);
  pinMode(pwmPin, OUTPUT);
  pinMode(stbyPin, OUTPUT);
  pinMode(modePin, INPUT_PULLDOWN);

  analogWriteResolution(8);
  analogWriteFrequency(pwmPin, 25000);
  analogWrite(pwmPin, 0);
  enableDriver(true);

  int raw = readPot();
  posFilt = raw;
  lastPos = raw;
  targetCounts = raw;
  coast();

  Serial.begin(115200);
  delay(100);
  Serial.println("Throttle ready (Weighted Motion + Joystick + Virtual Detent)");
}

void loop() {
  // ---- Mode input ----
  bool atMode = digitalRead(modePin);
  if (atMode != lastMode) {
    Serial.print("[MODE] Switched to ");
    Serial.println(atMode ? "AUTO" : "MANUAL");
    lastMode = atMode;
  }

  // ---- Position & velocity ----
  int posRaw = readPot();
  posFilt += alphaPos * (posRaw - posFilt);

  static uint32_t lastMs = millis();
  uint32_t now = millis();
  float dt = max<uint32_t>(1, now - lastMs);
  vel = (posFilt - lastPos) / dt;   // counts per ms
  lastPos = posFilt;
  lastMs = now;

  // ---- Throttle + reverse mapping ----
  bool  reverseActive = false;
  float reverseAmount = 0.0f;
  int   joyThrottle   = mapThrottleAndReverse(posFilt, reverseActive, reverseAmount);

  // Main throttle axis uses only top 50% of physical travel
  Joystick.sliderLeft(joyThrottle);                 // “Throttle Axis” in MSFS (0–1023)

  // Optional: expose reverse thrust amount on second slider axis
  // Map 0.0–1.0 → 0–1023
  int joyReverse = (int)(reverseAmount * 1023.0f);
  Joystick.sliderRight(joyReverse);

  // Log transitions into/out of reverse zone
  if (reverseActive != lastReverseActive) {
    lastReverseActive = reverseActive;
    if (DEBUG_REVERSE) {
      Serial.print("[REV] ");
      Serial.println(reverseActive ? "ENTER reverse zone" : "EXIT reverse zone");
    }
  }

  // ---- Serial target ----
  readSerialTarget();

  // ---- Mode behavior ----
  bool haveATcmd = (millis() - lastSerialMs) <= serialTimeoutMs;

  if (atMode) {
    if (haveATcmd) {
      runControlToTarget();
    } else {
      softHold();
    }
  } else {
    coast();  // free motion when manual
  }

  // ---- General debug output ----
  if (DEBUG_MODE && (millis() % 500 < 15)) {
    Serial.print("[POS] ");
    Serial.print(posFilt, 1);
    Serial.print(" tgt=");
    Serial.print(targetCounts);
    Serial.print(" vel=");
    Serial.print(vel, 3);
    Serial.print(" joyFwd=");
    Serial.print(joyThrottle);
    Serial.print(" joyRev=");
    Serial.println(joyReverse);
  }

  delay(10);
}