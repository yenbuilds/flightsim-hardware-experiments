// Teensy 4.1 + TB6612FNG simple back-and-forth test
// Wiring:
//   8  -> AIN1
//   9  -> AIN2
//   10 -> PWMA (PWM pin)
//   7  -> STBY (or tie HIGH to 5V)
//   Motor -> A01/A02
//   VCC -> 5V (logic)
//   VM  -> 5V (motor power)
//   GND -> common ground (Teensy + supply)

const int IN1  = 8;
const int IN2  = 9;
const int PWM  = 10;
const int STBY = 7;

// motion timing
const int rampDelay   = 15;    // ms per step in ramp
const int holdTime    = 1200;  // ms hold full power
const int stopTime    = 400;   // ms stop between moves
const int maxPWM      = 255;   // full speed

void setup() {
  pinMode(IN1, OUTPUT);
  pinMode(IN2, OUTPUT);
  pinMode(PWM, OUTPUT);
  pinMode(STBY, OUTPUT);

  digitalWrite(STBY, HIGH);           // enable driver
  analogWriteFrequency(PWM, 20000);   // ultrasonic PWM
}

// helper functions
void driveForward(int pwm) {
  digitalWrite(IN1, HIGH);
  digitalWrite(IN2, LOW);
  analogWrite(PWM, pwm);
}

void driveReverse(int pwm) {
  digitalWrite(IN1, LOW);
  digitalWrite(IN2, HIGH);
  analogWrite(PWM, pwm);
}

void stopMotor() {
  analogWrite(PWM, 0);
  digitalWrite(IN1, LOW);
  digitalWrite(IN2, LOW);
}

void loop() {
  // Ramp up forward
  for (int p = 0; p <= maxPWM; p += 20) {
    driveForward(p);
    delay(rampDelay);
  }
  driveForward(maxPWM);
  delay(holdTime);

  // Stop
  stopMotor();
  delay(stopTime);

  // Ramp up reverse
  for (int p = 0; p <= maxPWM; p += 20) {
    driveReverse(p);
    delay(rampDelay);
  }
  driveReverse(maxPWM);
  delay(holdTime);

  // Stop again
  stopMotor();
  delay(stopTime);
}
