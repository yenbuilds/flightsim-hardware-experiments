# Boeing 737 motorised throttle

This is a mini motorised airplane throttle project. Nothing starts on its own, and the bridge only moves the levers when you explicitly enable it.

The supplied profile is `boeing-737` (also `737` and `pmdg-737`). It maps a simulator throttle value to the physical lever from the 45% active floor to full travel:

| Sim throttle | Pot target | Serial command |
| --- | --- | --- |
| 0% | 461 | `T0461` |
| 50% | 737 | `T0737` |
| 100% | 1013 | `T1013` |

## What you need

- Teensy 4.1.
- A motorised throttle mechanism with a potentiometer mechanically linked to the lever.
- TB6612FNG motor-driver breakout, using channel A.
- A maintained 3.3 V A/T-enable signal for Teensy pin 2 (normally a latching toggle switch).
- A motor power supply matched to the motor. The old build used roughly 5-9 V; confirm the motor current and voltage before using that range.
- USB connection from the Teensy to the PC.
- Arduino IDE with Teensy support.
- 64-bit Python 3.8+ for the MSFS bridge. Node.js 18+ is only needed if you want to write your own Node bridge.

Optional: three red/green gear LEDs, each with suitable current limiting.

## Wiring assumed by the firmware

| Teensy 4.1 pin | Connect to | Notes |
| --- | --- | --- |
| A0 | Throttle-pot wiper | The pot must stay between 0 and 3.3 V. |
| 8 | TB6612 `AIN1` | Motor direction. |
| 9 | TB6612 `AIN2` | Motor direction. |
| 10 | TB6612 `PWMA` | PWM motor speed. |
| 7 | TB6612 `STBY` | The firmware drives this high to enable the driver. |
| 2 | A/T-enable input | High = local A/T motor enable; uses Teensy's internal pull-down. A latching toggle switch is the normal choice. Do not apply more than 3.3 V. |
| 35 / 36 | Nose gear red / green LED | Optional; use resistors. |
| 37 / 38 | Left gear red / green LED | Optional; use resistors. |
| 39 / 40 | Right gear red / green LED | Optional; use resistors. |

Connect the motor to TB6612 `A01` and `A02`. Connect Teensy ground, TB6612 ground, pot ground, and motor-supply ground together.

The Teensy GPIO pins are 3.3 V. The TB6612 input-high threshold is specified as 0.7 × its logic supply, so use a 3.3 V-compatible logic supply for the driver or add level shifting. Do not assume a 5 V `VCC` on the driver will reliably accept the Teensy's 3.3 V control signals. See the [TB6612FNG datasheet](https://toshiba.semicon-storage.com/info/datasheet_en_20141001.pdf?did=10660).

`VM` is the motor supply; `VCC` is the driver logic supply. They are not interchangeable. Do not power the motor from a Teensy GPIO pin or USB 5 V rail.

## Flash the Teensy

Open `src/teensy/autothrottle/autothrottle.ino` in Arduino IDE.

1. Select **Teensy 4.1**.
2. Select a USB type that includes both **Serial** and **Joystick**. The sketch uses `Serial` for host commands and `Joystick.sliderLeft()` for the throttle axis.
3. Upload with motor power disconnected.
4. Open the serial monitor at 115200 baud. It should print `Throttle ready...`.

Before connecting the motor, move the lever by hand and confirm the pot readings/calibration. The current defaults are 10 at one end and 1013 at the other. To print live readings, temporarily set `DEBUG_MODE` to `true`, then turn it back off before normal use. If the reading runs the wrong way, change `invertPot` in the sketch before testing the motor.

## Run it with MSFS

`bridge/msfs_teensy_bridge.py` reads the standard engine-throttle values from SimConnect, averages engines 1 and 2, and sends the result to the Teensy. It uses the same 461-to-1013 calibration as the firmware.

Start with a dry run. It talks to MSFS and prints the commands it would send, but it does not open the Teensy serial port or move anything.

```powershell
py -3 -m venv .venv
.venv\Scripts\Activate
py -m pip install -r requirements-msfs-bridge.txt
py bridge\msfs_teensy_bridge.py
```

With motor power disconnected and the A/T switch in MANUAL, check that 0%, 50%, and 100% give roughly `T0461`, `T0737`, and `T1013`. When those look right, reconnect motor power, keep the switch in MANUAL, and run:

```powershell
py bridge\msfs_teensy_bridge.py --port COM10 --enable-motion
```

At startup it sends the calibration to the Teensy, then waits for three good readings before it sends a target. The Teensy's pin-2 input controls whether it drives the motor; it is independent of the simulator's A/T state. The bridge reads only engine-throttle positions and does not know whether the sim's A/T is engaged. A momentary pushbutton enables motor control only while held—the current firmware does not latch it.

Run either this bridge or a Node bridge, not both. Two programs must not write to the same Teensy port.

The requirements file pins `SimConnect==0.4.26`, the package used by the original bridge. See its [package documentation](https://pypi.org/project/SimConnect/).

## If you want to use Node instead

Use this route only when you already have a Node program that can provide the throttle values. From this folder:

```powershell
npm.cmd test
npm install serialport
```

Set the serial port and enable output only when you are ready to move the real lever:

```powershell
$env:AT_ENABLE = '1'
$env:AT_TEENSY_PORT = 'COM10'
$env:THROTTLE_PROFILE = 'boeing-737'
```

Settings you may need to change:

| Setting | Default | Meaning |
| --- | --- | --- |
| `AT_POT_MIN` | `10` | Calibrated low pot count. |
| `AT_POT_MAX` | `1013` | Calibrated high pot count. |
| `AT_RAW_MAX` | `16383` | Maximum incoming simulator throttle value. |
| `AT_BAUD` | `115200` | USB serial speed. |
| `AT_SEND_INTERVAL_MS` | `80` | Minimum time between throttle sends. |
| `AT_MIN_DELTA` | `2` | Minimum count change before an early resend. |
| `AT_DEBUG` | `0` | Host-side logging. |

`AT_POT_MIN` and `AT_POT_MAX` must match the physical quadrant. The Node side sends them to the Teensy at startup, along with the 461-count gate.

## Writing a Node bridge

Your telemetry code must call this module whenever it receives engine-throttle values. `thr1` and `thr2` must use the raw scale configured by `AT_RAW_MAX` (16383 by default).

```js
const throttle = require('./src/backend/aircraft/throttle');
const serial = require('./src/teensy/autothrottle-serial');

if (!serial.initTeensySerial()) {
  throw new Error('Teensy serial port was not opened');
}

serial.sendDetentLayout(); // D,10,1013,461 with default calibration
serial.sendRangeMode();    // R,half for the 737 profile

function onThrottleUpdate(thr1, thr2) {
  const snapshot = throttle.computeThrottleSnapshot({ thr1, thr2 }, false);
  serial.sendThrottleCommand(snapshot);
}
```

There is no ready-made Node program here. This is the API to call if you are replacing the Python bridge.

## First powered test

1. Keep the motor supply off and confirm the Teensy appears as both serial and joystick USB devices.
2. ??Set the pot limits and check the lever does not report outside them??
3. Connect motor power with the ??lever near the 461-count floor??.
4. Send a small target change first and check the direction. If it is wrong, reverse the motor leads or fix the direction logic before it reaches a mechanical stop.
5. Start the Python bridge in dry-run mode, then use `--enable-motion` only after the targets are correct.

The unfinished A320 and unused 777 profiles are in `disabled-profiles/` and cannot be selected.
