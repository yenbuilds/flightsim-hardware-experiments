// ======================================================
//  Teensy 4.1 Motorized Throttle (TB6612FNG)
//  Serial A/T + Manual + Mode Echo + Weighted Motion + Joystick Axis
//  + Virtual Detent at 45% (upper travel used for throttle)
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
#include <stdlib.h>
#include <string.h>

// ---------------- Debug Toggles ----------------
#define DEBUG_MODE     false    // General logging (pos/target/vel)
#define DEBUG_PID      false    // Detailed PID term logging
#define DEBUG_SERIAL   false    // Serial command updates

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
int   potMinCfg   = 10;
int   potMaxCfg   = 1013;

// Virtual detent: default active floor and manual tactile gate
const float detentFrac   = 0.45f; // Matches the Boeing 737 active floor profile
int   detentCounts       = 0;

enum TravelMode { TRAVEL_HALF = 0, TRAVEL_FULL = 1, TRAVEL_BOTTOM_CLAMP = 2 };
volatile TravelMode travelMode = TRAVEL_HALF; // Boeing 737 forward-throttle travel

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

// Detent layout (populated from host config). Max gates keeps RAM small.
const int MAX_GATES = 8;
int gateCounts[MAX_GATES];
int gateCount = 0;
int detentLockIdx = -1;


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
void recomputeDetentCounts() {
  detentCounts = potMinCfg + (int)(((potMaxCfg - potMinCfg) * detentFrac) + 0.5f);
}

void loadDefaultGates() {
  recomputeDetentCounts();
  gateCount = 1;
  gateCounts[0] = detentCounts;
  detentLockIdx = -1;
}

int readPot() {
  int r = analogRead(potPin);
  if (invertPot) r = 1023 - r;
  r = constrain(r, potMinCfg, potMaxCfg);
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


// ======================================================
// ---------------- Gear LED state ----------------------
// ======================================================

// Last known safe states
char lastN = 'U';
char lastL = 'U';
char lastR = 'U';

// Apply state with valid-character filtering.
void applyGearLED(char c, char &lastState, int redPin, int greenPin) {
  if (c == 'U' || c == 'T' || c == 'D') {
    lastState = c;
  }
  // else ignore glitch/noise and keep lastState

  if (lastState == 'T') {
    digitalWrite(redPin, HIGH);
    digitalWrite(greenPin, LOW);
  } else if (lastState == 'D') {
    digitalWrite(redPin, LOW);
    digitalWrite(greenPin, HIGH);
  } else { // U
    digitalWrite(redPin, LOW);
    digitalWrite(greenPin, LOW);
  }
}


// ---------------- MANUAL-mode detent feel -------------
void applyManualDetent() {
  const float innerBand = 5.0f;   // lock/brake band
  const float outerBand = 18.0f;  // soft pull toward nearest gate

  if (gateCount <= 0) {
    coast();
    detentLockIdx = -1;
    return;
  }

  // Find nearest gate
  int nearestIdx = -1;
  float nearestDist = 1e9f;
  float nearestVal = 0;
  for (int i = 0; i < gateCount; i++) {
    float dist = fabs(posFilt - (float)gateCounts[i]);
    if (dist < nearestDist) {
      nearestDist = dist;
      nearestIdx = i;
      nearestVal = gateCounts[i];
    }
  }

  // Virtual midpoint gate for bottom-clamp mode so user feels the max point
  if (travelMode == TRAVEL_BOTTOM_CLAMP) {
    float midDist = fabs(posFilt - (float)detentCounts);
    if (midDist < nearestDist) {
      nearestDist = midDist;
      nearestIdx = -2; // virtual gate identifier
      nearestVal = detentCounts;
    }
  }

  if (nearestIdx == -1) {
    coast();
    detentLockIdx = -1;
    return;
  }

  if (nearestDist > outerBand) {
    detentLockIdx = -1;
    coast();
    return;
  }

  if (nearestDist <= innerBand) {
    detentLockIdx = nearestIdx;
    brake();
    return;
  }

  if (detentLockIdx == nearestIdx) {
    brake();
    return;
  }

  // Pull toward nearest gate with variable strength
  float norm = (nearestDist - innerBand) / (outerBand - innerBand);
  if (norm < 0.0f) norm = 0.0f;
  if (norm > 1.0f) norm = 1.0f;

  float strength = 1.0f - norm;
  int pwm = 80 + (int)(strength * 90);
  if (pwm > 180) pwm = 180;

  if (posFilt < nearestVal) driveForward(pwm);
  else                      driveReverse(pwm);
}


// ---------------- Throttle mapping --------------------
int mapThrottleOutput(float posCounts) {
  float p = posCounts;
  if (p < potMinCfg) p = potMinCfg;
  if (p > potMaxCfg) p = potMaxCfg;

  if (travelMode == TRAVEL_FULL) {
    int val = map((int)p, potMinCfg, potMaxCfg, 0, 1023);
    if (val < 0) val = 0;
    if (val > 1023) val = 1023;
    return val;
  }

  if (travelMode == TRAVEL_BOTTOM_CLAMP) {
    if (p <= detentCounts) {
      int val = map((int)p, potMinCfg, detentCounts, 0, 1023);
      if (val < 0) val = 0;
      if (val > 1023) val = 1023;
      return val;
    }
    return 1023; // clamp above midpoint to max
  }

  if (p < detentCounts) {
    return 0;
  }

  int val = map((int)p, detentCounts, potMaxCfg, 0, 1023);
  if (val < 0) val = 0;
  if (val > 1023) val = 1023;
  return val;
}


// ======================================================
// ---------------- Serial command handler --------------
// ======================================================
void applyDetentConfig(int newPotMin, int newPotMax, int *gates, int count) {
  if (newPotMin < 0 || newPotMax > 1023 || newPotMax <= newPotMin) return;

  potMinCfg = newPotMin;
  potMaxCfg = newPotMax;
  recomputeDetentCounts();
  detentLockIdx = -1;

  if (count > MAX_GATES) count = MAX_GATES;
  gateCount = 0;
  for (int i = 0; i < count; i++) {
    int g = constrain(gates[i], potMinCfg, potMaxCfg);
    gateCounts[gateCount++] = g;
  }

  if (gateCount == 0) {
    loadDefaultGates();
  }

  if (DEBUG_SERIAL) {
    Serial.print("[CMD] Detents: potMin=");
    Serial.print(potMinCfg);
    Serial.print(" potMax=");
    Serial.print(potMaxCfg);
    Serial.print(" gates=");
    for (int i = 0; i < gateCount; i++) {
      Serial.print(gateCounts[i]);
      if (i < gateCount - 1) Serial.print(',');
    }
    Serial.println();
  }
}

void handleDetentMessage(char *payload) {
  // Expect: ,potMin,potMax,gate1,gate2,... (comma separated). pot/gate units are raw counts.
  int vals[MAX_GATES + 2];
  int n = 0;
  char *p = payload;
  if (*p == ',') p++; // tolerate leading comma

  while (*p && n < (MAX_GATES + 2)) {
    char *end = nullptr;
    long parsed = strtol(p, &end, 10);
    if (end == p || (*end != ',' && *end != '\0')) return;
    if (parsed < -32768L || parsed > 32767L) return;
    vals[n++] = (int)parsed;
    if (*end == '\0') break;
    p = end + 1;
  }

  if (n < 3) return; // need potMin, potMax, at least one gate

  int gateVals[MAX_GATES];
  int gateN = n - 2;
  for (int i = 0; i < gateN && i < MAX_GATES; i++) {
    gateVals[i] = vals[i + 2];
  }

  applyDetentConfig(vals[0], vals[1], gateVals, gateN);
}

void handleRangeMessage(char *payload) {
  // Expect: ,half or ,full or ,bottom
  while (*payload == ',') payload++;
  if (strcmp(payload, "full") == 0) {
    travelMode = TRAVEL_FULL;
  } else if (strcmp(payload, "bottom") == 0) {
    travelMode = TRAVEL_BOTTOM_CLAMP;
  } else if (strcmp(payload, "half") == 0) {
    travelMode = TRAVEL_HALF;
  } else return;
  if (DEBUG_SERIAL) {
    Serial.print("[CMD] Travel mode: ");
    if (travelMode == TRAVEL_FULL) Serial.println("full");
    else if (travelMode == TRAVEL_BOTTOM_CLAMP) Serial.println("bottom");
    else Serial.println("half");
  }
}

void readSerialTarget() {
  while (Serial.available()) {
    static char buf[64];
    static int idx = 0;
    char c = Serial.read();

    if (c == '\n' || c == '\r') {
      buf[idx] = 0;

      if (buf[0] == 'T') {
        char *end = nullptr;
        long parsed = strtol(buf + 1, &end, 10);
        if (end != (buf + 1) && *end == '\0' && parsed >= 0 && parsed <= 1023) {
          targetCounts = constrain((int)parsed, detentCounts, potMaxCfg);
          lastSerialMs = millis();
          if (DEBUG_SERIAL) {
            Serial.print("[CMD] New target: ");
            Serial.println(targetCounts);
          }
        }
      }

      else if (buf[0] == 'D') {
        handleDetentMessage(buf + 1);
      }

      else if (buf[0] == 'R') {
        handleRangeMessage(buf + 1);
      }

      else if (buf[0] == 'G' && idx == 4) {
        char n = buf[1];
        char l = buf[2];
        char r = buf[3];

        applyGearLED(n, lastN, led1Pin, led2Pin);
        applyGearLED(l, lastL, led3Pin, led4Pin);
        applyGearLED(r, lastR, led5Pin, led6Pin);

        if (DEBUG_SERIAL) {
          Serial.print("[CMD] Gear LEDs NLR: ");
          Serial.print(lastN);
          Serial.print(lastL);
          Serial.println(lastR);
        }
      }

      idx = 0;
    }

    else if (idx < (int)sizeof(buf) - 1) {
      buf[idx++] = c;
    }

    else idx = 0;
  }
}


// ======================================================
// ---------------- PID control loop (AUTO) -------------
// ======================================================
void runControlToTarget() {
  float e = (float)targetCounts - posFilt;

  if (fabs(e) <= deadband) {
    brake();
    iTerm *= 0.9f;
    lastPWM = 0;
    if (DEBUG_PID) {
      Serial.print("[PID] Near target e=");
      Serial.println(e, 2);
    }
    return;
  }

  float pTerm = Kp * e;
  iTerm += Ki * e;
  iTerm = constrain(iTerm, -iClamp, iClamp);
  float dTerm = -Kd * vel;
  float u = pTerm + iTerm + dTerm;

  int pwm = (int)fabs(u);
  pwm = constrain(pwm, 0, maxPWM);

  if (pwm > 0 && pwm < minPWM_AT) pwm = minPWM_AT;

  float eAbs = fabs(e);
  const float slowDownDist = 120.0f;
  if (eAbs < slowDownDist) {
    int dynamicMax =
      minPWM_AT + (int)((maxPWM - minPWM_AT) * (eAbs / slowDownDist));
    if (dynamicMax < minPWM_AT) dynamicMax = minPWM_AT;
    pwm = min(pwm, dynamicMax);
  }

  if (pwm > lastPWM) {
    int delta = pwm - lastPWM;
    if (delta > pwmRampUpStep) pwm = lastPWM + pwmRampUpStep;
  } else {
    int delta = lastPWM - pwm;
    if (delta > pwmRampDownStep) pwm = lastPWM - pwmRampDownStep;
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

  recomputeDetentCounts();
  loadDefaultGates();

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
  bool atMode = digitalRead(modePin);

  if (atMode != lastMode) {
    Serial.print("[MODE] Switched to ");
    Serial.println(atMode ? "AUTO" : "MANUAL");
    lastMode = atMode;
  }

  int posRaw = readPot();
  posFilt += alphaPos * (posRaw - posFilt);

  static uint32_t lastMs = millis();
  uint32_t now = millis();
  float dt = max<uint32_t>(1, now - lastMs);
  vel = (posFilt - lastPos) / dt;
  lastPos = posFilt;
  lastMs = now;

  int joyThrottle = mapThrottleOutput(posFilt);
  Joystick.sliderLeft(joyThrottle);

  readSerialTarget();

  bool haveATcmd = (millis() - lastSerialMs) <= serialTimeoutMs;

  if (atMode) {
    if (haveATcmd) runControlToTarget();
    else           softHold();
  } else {
    applyManualDetent();
  }

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

  delay(10);
}
