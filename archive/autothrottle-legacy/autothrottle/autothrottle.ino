// ------------------------------------------------------------
// Teensy Diagnostic: Read RC-filtered PWM from Mobiflight
// + Robust decode of 2-bit packed strobe/park
// Calibrated to your measured RAW bands:
//   0, 150, 315, 468
// ------------------------------------------------------------

// const int mfPinPWM = A1;    // A1 == physical pin 15 on Teensy 4.1

// // Calibrated thresholds between the 4 RAW clusters:
// const int TH01 = 75;   // between 0 and ~150
// const int TH12 = 230;  // between ~150 and ~315
// const int TH23 = 400;  // between ~315 and ~468

// void setup() {
//   Serial.begin(115200);
//   delay(500);

//   Serial.println("=== Mobiflight PWM Diagnostic + Calibrated Decode ===");
//   Serial.print("Reading from analog pin: ");
//   Serial.println(mfPinPWM);

//   analogReadResolution(10);   // 0–1023
//   analogReadAveraging(16);    // smooth
// }

// void loop() {
//   // ----- Read analog voltage from RC filter -----
//   int raw = analogRead(mfPinPWM);   // 0–~470 in your case

//   // ----- Convert RAW into 0..3 bucket using thresholds -----
//   int bucket;
//   if (raw < TH01)       bucket = 0;
//   else if (raw < TH12)  bucket = 1;
//   else if (raw < TH23)  bucket = 2;
//   else                  bucket = 3;

//   // ----- Decode bits from bucket -----
//   // MF side: packed = strobe*1 + park*2
//   bool strobe = (bucket & 0x01) != 0;
//   bool park   = (bucket & 0x02) != 0;

//   // If either one is logically inverted vs what you expect,
//   // you can flip it here:
//   // strobe = !strobe;
//   // park   = !park;

//   // ----- Print everything -----
//   Serial.print("RAW=");
//   Serial.print(raw);
//   Serial.print("  BUCKET=");
//   Serial.print(bucket);
//   Serial.print("  STROBE=");
//   Serial.print(strobe ? 1 : 0);
//   Serial.print("  PARK=");
//   Serial.println(park ? 1 : 0);

//   delay(50);   // 20 Hz
// }


// ======================================================
//  Teensy 4.1 Motorized Throttle (TB6612FNG)
//  Serial A/T + Manual + Mode Echo + Weighted Motion + Joystick Axis
//  + Virtual Detent at 50% (top 50% used for throttle)
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
#define DEBUG_MODE     true     // General logging (pos/target/vel)
#define DEBUG_PID      true    // Detailed PID term logging
#define DEBUG_SERIAL   true     // Serial command updates

// ---------------- Pins ----------------
const int potPin  = A0; // this is pin 14 on Teensy 4.1
const int in1Pin  = 8;
const int in2Pin  = 9;
const int pwmPin  = 10;
const int stbyPin = 7;
const int modePin = 2;     // HIGH = A/T active

// ---------------- LED pins -------------
const int led1Pin = 35;   // LED 1A (e.g., COLOR1)
const int led2Pin = 36;   // LED 1B (e.g., COLOR2)

const int led3Pin = 37;   // LED 2A (e.g., COLOR1)
const int led4Pin = 38;   // LED 2B (e.g., COLOR2)

const int led5Pin = 39;   // LED 3A (e.g., COLOR1)
const int led6Pin = 40;   // LED 3B (e.g., COLOR2)


// ---------------- Tuning / Limits ----------------
bool  invertPot   = false;
const int potMin  = 10;
const int potMax  = 1013;

// Virtual detent: geometric middle of calibrated range
const float detentFrac   = 0.5f; // 0.5 = middle of travel
const int   detentCounts = potMin + (int)((potMax - potMin) * detentFrac);

// Slightly more smoothing for heavy feel
const float alphaPos = 0.12;

// Tolerance for PID near target (AUTO)
// (smaller so overshoot gets corrected)
const int   deadband = 2;

// Soft-hold tolerance only used when A/T target has timed out
const int   holdBand = 10;

// PWM limits and ramp
const int   minPWM_AT       = 55;  // stronger min torque for A/T
const int   minPWM          = 35;  // general min (detent / softHold)
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

// Detent state (for MANUAL mode hysteresis)
bool detentLocked = false;

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

// Gentle hold used ONLY when we have no recent A/T command
void softHold() {
  float e = (float)targetCounts - posFilt;

  if (fabs(vel) < vThresh && fabs(e) <= holdBand) {
    coast();
  } else {
    int tiny = minPWM;
    if (e > 0) driveForward(tiny);
    else       driveReverse(tiny);
  }
}

// ---------------- MANUAL-mode detent feel -------------
// Only used when A/T is OFF.
// Strong but stable detent using a small state machine:
// - Outside outer band: no detent
// - Inside inner band: locked in center, no driving
// - Between bands and not locked: strong spring toward center
void applyManualDetent() {
  const float innerBand = 5.0f;   // counts, tight core
  const float outerBand = 18.0f;  // counts, total detent width

  float eDet = (float)detentCounts - posFilt; // >0 = below detent, <0 = above
  float dist = fabs(eDet);

  // 1) Completely outside detent region → no detent, unlocked
  if (dist > outerBand) {
    detentLocked = false;
    coast();
    return;
  }

  // 2) Deep in the center region → lock and do not drive
  if (dist <= innerBand) {
    detentLocked = true;
    brake();   // electrical stickiness at center
    return;
  }

  // 3) Between innerBand and outerBand
  if (detentLocked) {
    // Still "in" the detent, keep it stuck without driving.
    brake();
    return;
  }

  // Not locked, inside detent ring → strong spring toward center.

  // Map [innerBand .. outerBand] -> [0 .. 1]
  float norm = (dist - innerBand) / (outerBand - innerBand);
  if (norm < 0.0f) norm = 0.0f;
  if (norm > 1.0f) norm = 1.0f;

  float strength = 1.0f - norm;   // 1 at inner edge, 0 at outer edge

  int pwm = 80 + (int)(strength * 90);   // ~80..170
  if (pwm > 180) pwm = 180;

  if (eDet > 0) {
    driveForward(pwm);
  } else {
    driveReverse(pwm);
  }
}

// ---------------- Throttle mapping (top 50%) ----------
// Map physical position (counts) into:
// - Forward throttle axis (0–1023) using ONLY top 50% of travel
// - Below detent, throttle axis stays at 0 (ignored for in-flight throttle)
int mapThrottleTopHalf(float posCounts) {
  float p = posCounts;
  if (p < potMin) p = potMin;
  if (p > potMax) p = potMax;

  if (p < detentCounts) {
    return 0;  // idle / reverse region (axis-wise)
  }

  int val = map((int)p, detentCounts, potMax, 0, 1023);
  if (val < 0) val = 0;
  if (val > 1023) val = 1023;
  return val;
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
        // A/T is only allowed in top 50%: clamp to [detentCounts .. potMax]
        v = constrain(v, detentCounts, potMax);
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
// ---------------- PID control loop (AUTO) -------------
// ======================================================
// Stronger A/T behaviour:
// - Deadband just keeps it from buzzing right on target
// - NO softHold here: always actively pull back if outside deadband
// - Stronger min PWM so it can overcome friction/manual shoves
void runControlToTarget() {
  float e = (float)targetCounts - posFilt;

  // Very close to target → just brake and lightly bleed integral
  if (fabs(e) <= deadband) {
    brake();
    iTerm *= 0.9f;   // bleed integral a bit
    lastPWM = 0;
    if (DEBUG_PID) {
      Serial.print("[PID] Near target e=");
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

  // Enforce stronger minimum torque in AUTO
  if (pwm > 0 && pwm < minPWM_AT) pwm = minPWM_AT;

  // --- Distance-based speed cap near target (smoother arrival) ---
  float eAbs = fabs(e);
  const float slowDownDist = 120.0f; // counts: within this, start capping max speed
  if (eAbs < slowDownDist) {
    int dynamicMax = minPWM_AT + (int)((maxPWM - minPWM_AT) * (eAbs / slowDownDist));
    if (dynamicMax < minPWM_AT) dynamicMax = minPWM_AT;
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
  else            brake();
}

// ======================================================
// ---------------- Setup & main loop -------------------
// ======================================================
void setup() {
  pinMode(led1Pin, OUTPUT);
  pinMode(led2Pin, OUTPUT);

  pinMode(led3Pin, OUTPUT);
  pinMode(led4Pin, OUTPUT);

  pinMode(led5Pin, OUTPUT);
  pinMode(led6Pin, OUTPUT);

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
  bool atMode = digitalRead(modePin);   // HIGH = A/T, LOW = manual

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

  // ---- Throttle mapping (top 50% only) ----
  int joyThrottle = mapThrottleTopHalf(posFilt);
  Joystick.sliderLeft(joyThrottle);  // “Throttle Axis” in MSFS (0–1023)

  // ---- Serial target ----
  readSerialTarget();

  // ---- Mode behavior ----
  bool haveATcmd = (millis() - lastSerialMs) <= serialTimeoutMs;

  if (atMode) {
    // A/T ON: full PID, stronger minimum torque, no softHold
    if (haveATcmd) {
      runControlToTarget();
    } else {
      // No fresh A/T command → just gently hold last position
      softHold();
    }
  } else {
    // A/T OFF: free throttle but with a detent notch at the midpoint
    applyManualDetent();
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
    Serial.println(joyThrottle);
  }

  // ---- LED test blink ----
  // Uncomment to witness nuclear sun
 
    static bool ledState = true;

  digitalWrite(led1Pin, ledState ? HIGH : LOW); // Gear Nose, RED
  digitalWrite(led2Pin, ledState ? HIGH : LOW); // Gear Nose, GREEN

 digitalWrite(led3Pin, ledState ? HIGH : LOW); // Gear Left, RED
digitalWrite(led4Pin, ledState ? HIGH : LOW); // Gear Left, GREEN

   digitalWrite(led5Pin, ledState ? HIGH : LOW); // Gear Right, RED
   digitalWrite(led6Pin, ledState ? HIGH : LOW); // Gear Right, GREEN

   
  delay(10);
}
