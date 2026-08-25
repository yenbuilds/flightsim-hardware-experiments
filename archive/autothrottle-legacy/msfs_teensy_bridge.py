"""
===========================================================
   MSFS → Teensy Autothrottle Bridge (Python Side)
===========================================================

This script forms HALF of a 2-part autothrottle system:

   • **MSFS/SimConnect side (this file)**
   • **Teensy 4.1 motor-drive firmware (your Arduino sketch)**

Its job is to read the current *simulated throttle lever positions* from
Microsoft Flight Simulator (MSFS) and convert them into a **physical target
potentiometer position** for your motorised throttle quadrant.

--------------------------
HOW IT WORKS
--------------------------

1. Connects to MSFS using the SimConnect SDK.
2. Reads engine throttle lever positions:
       GENERAL_ENG_THROTTLE_LEVER_POSITION:1
       GENERAL_ENG_THROTTLE_LEVER_POSITION:2
   (values are 0–100 %, matching virtual throttle position)

3. Averages ENG1 & ENG2 → forms a single target percentage.

4. Converts that percentage into a *potentiometer count* that matches the
   calibrated Teensy pot range:
       POT_MIN … DETENT … POT_MAX

   Only the upper region [DETENT → POT_MAX] is used for autothrottle (A/T) so
   that the lower half of the physical travel is kept for manual control.

5. Sends a serial command over USB to the Teensy:
       T####\n
   where #### is the desired ADC count (e.g., T745)

6. The Teensy uses this to drive the motor via PID + H-bridge to move the
   physical throttle lever to match MSFS.

--------------------------
EXPECTED USAGE
--------------------------
• Run MSFS first, load an aircraft, start a flight.
• Ensure the Teensy is connected on SERIAL_PORT (e.g., COM10).
• Run this script.
• When A/T is active and the sim changes throttle,
  the motorised throttle will physically move.

--------------------------
TUNING PARAMETERS
--------------------------
• POT_MIN / POT_MAX → measured raw ADC range from Teensy.
• DETENT_FRAC       → must match Teensy firmware.
• UPDATE_HZ         → command frequency to Teensy (30 Hz default).

This script is intentionally simple and low-latency.

===========================================================
"""

import time
import serial
from SimConnect import SimConnect, AircraftRequests

# ---------------- CONFIG ----------------
SERIAL_PORT = "COM10"   # <- your Teensy COM port
BAUDRATE    = 115200
UPDATE_HZ   = 30

POT_MIN = 10
POT_MAX = 1013
DETENT_FRAC = 0.45   # must match Teensy
DETENT = int(POT_MIN + (POT_MAX - POT_MIN) * DETENT_FRAC)

def clamp(x, lo, hi):
    return lo if x < lo else hi if x > hi else x

# ---------------- INIT ----------------
ser = serial.Serial(SERIAL_PORT, BAUDRATE, timeout=1)
sm  = SimConnect()
aq  = AircraftRequests(sm, _time=0)

print("Python → Teensy autothrottle bridge running...")

# ---------------- MAIN LOOP ----------------
while True:
    try:
        thr1 = aq.get("GENERAL_ENG_THROTTLE_LEVER_POSITION:1")
        thr2 = aq.get("GENERAL_ENG_THROTTLE_LEVER_POSITION:2")

        if thr1 is None: thr1 = 0.0
        if thr2 is None: thr2 = thr1

        target_pct = clamp((thr1 + thr2) / 2.0, 0.0, 100.0)

        # Map sim 0–100% into [DETENT .. POT_MAX] (top half only)
        counts = int(DETENT + (target_pct / 100.0) * (POT_MAX - DETENT))

        cmd = f"T{counts}\n"
        ser.write(cmd.encode())

        print(f"MSFS {target_pct:6.2f}%  →  {cmd.strip()}")

        time.sleep(1.0 / UPDATE_HZ)

    except KeyboardInterrupt:
        print("\nStopped by user.")
        break
    except Exception as e:
        print("Error:", e)
        time.sleep(0.2)
