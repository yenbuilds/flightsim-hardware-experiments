"""Small MSFS-to-Teensy bridge for the Boeing 737 throttle.

It reads the two standard engine-throttle values, averages them, and sends a
``T####`` target to the Teensy. By default it only prints targets. It will not
open the serial port or move the levers without ``--enable-motion --port COMx``.
Do not run it with a process using autothrottle-serial.js.
"""

from __future__ import annotations

import argparse
import math
import sys
import time
from dataclasses import dataclass
from typing import Optional


DEFAULT_POT_MIN = 10
DEFAULT_POT_MAX = 1013
DEFAULT_DETENT_FRACTION = 0.45
DEFAULT_BAUD = 115200
DEFAULT_UPDATE_HZ = 12.5
DEFAULT_MIN_DELTA = 2
DEFAULT_MIN_INTERVAL_MS = 80
REQUIRED_VALID_SAMPLES = 3
SERIAL_STARTUP_SETTLE_SECONDS = 1.0


def finite_number(value: object) -> Optional[float]:
    """Return a finite number, rejecting booleans and malformed telemetry."""
    if isinstance(value, bool):
        return None
    try:
        number = float(value)
    except (TypeError, ValueError):
        return None
    return number if math.isfinite(number) else None


def clamp(value: float, lower: float, upper: float) -> float:
    return max(lower, min(upper, value))


def detent_counts(pot_min: int, pot_max: int, detent_fraction: float) -> int:
    if not (0 <= pot_min < pot_max <= 1023):
        raise ValueError("pot limits must satisfy 0 <= min < max <= 1023")
    if not 0 <= detent_fraction <= 1:
        raise ValueError("detent fraction must be between 0 and 1")
    return round(pot_min + (pot_max - pot_min) * detent_fraction)


def average_throttle_percent(thr1: object, thr2: object) -> Optional[float]:
    """Return the valid two-engine average, or None when engine 1 is invalid.

    Engine 2 may be missing while an aircraft is loading; in that case use the
    first engine rather than commanding an artificial move to the idle gate.
    """
    first = finite_number(thr1)
    if first is None:
        return None
    second = finite_number(thr2)
    if second is None:
        second = first
    return clamp((first + second) / 2, 0, 100)


def map_percent_to_counts(percent: float, pot_min: int, pot_max: int, detent_fraction: float) -> int:
    value = finite_number(percent)
    if value is None:
        raise ValueError("throttle percent must be a finite number")
    detent = detent_counts(pot_min, pot_max, detent_fraction)
    return round(detent + (clamp(value, 0, 100) / 100) * (pot_max - detent))


def format_target_command(counts: int) -> str:
    if not isinstance(counts, int) or isinstance(counts, bool) or not 0 <= counts <= 1023:
        raise ValueError("target counts must be an integer between 0 and 1023")
    return f"T{counts:04d}\n"


def format_layout_command(pot_min: int, pot_max: int, detent_fraction: float) -> str:
    return f"D,{pot_min},{pot_max},{detent_counts(pot_min, pot_max, detent_fraction)}\n"


@dataclass
class SendState:
    counts: Optional[int] = None
    timestamp: float = 0.0


def should_send(
    target: int,
    state: SendState,
    now: float,
    min_delta: int,
    min_interval_ms: int,
) -> bool:
    if state.counts is None:
        return True
    elapsed_ms = (now - state.timestamp) * 1000
    if abs(target - state.counts) < min_delta and elapsed_ms < min_interval_ms * 2:
        return False
    return elapsed_ms >= min_interval_ms


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", help="Teensy serial port, for example COM10")
    parser.add_argument("--baud", type=int, default=DEFAULT_BAUD)
    parser.add_argument("--update-hz", type=float, default=DEFAULT_UPDATE_HZ)
    parser.add_argument("--pot-min", type=int, default=DEFAULT_POT_MIN)
    parser.add_argument("--pot-max", type=int, default=DEFAULT_POT_MAX)
    parser.add_argument("--detent-fraction", type=float, default=DEFAULT_DETENT_FRACTION)
    parser.add_argument("--min-delta", type=int, default=DEFAULT_MIN_DELTA)
    parser.add_argument("--min-interval-ms", type=int, default=DEFAULT_MIN_INTERVAL_MS)
    parser.add_argument(
        "--enable-motion",
        action="store_true",
        help="open the serial port and send motor targets (otherwise dry-run only)",
    )
    args = parser.parse_args()

    if args.enable_motion and not args.port:
        parser.error("--enable-motion requires --port")
    if args.baud <= 0 or args.update_hz <= 0:
        parser.error("--baud and --update-hz must be positive")
    if args.min_delta < 0 or args.min_interval_ms < 0:
        parser.error("--min-delta and --min-interval-ms cannot be negative")
    try:
        detent_counts(args.pot_min, args.pot_max, args.detent_fraction)
    except ValueError as error:
        parser.error(str(error))
    return args


def load_simconnect():
    try:
        from SimConnect import AircraftRequests, SimConnect
    except ImportError as error:
        raise RuntimeError(
            "SimConnect is not installed. Run: py -m pip install -r requirements-msfs-bridge.txt"
        ) from error
    return SimConnect, AircraftRequests


def open_serial(port_name: str, baud: int):
    try:
        import serial
    except ImportError as error:
        raise RuntimeError(
            "pyserial is not installed. Run: py -m pip install -r requirements-msfs-bridge.txt"
        ) from error
    return serial.Serial(port_name, baud, timeout=1, write_timeout=1)


def run() -> int:
    args = parse_args()
    mode = "MOTION ENABLED" if args.enable_motion else "DRY RUN"
    detent = detent_counts(args.pot_min, args.pot_max, args.detent_fraction)
    print(f"MSFS bridge: {mode}; range {args.pot_min}..{args.pot_max}; active floor {detent}.")
    if not args.enable_motion:
        print("No serial port will be opened. Press Ctrl+C after confirming reported targets.")
    else:
        print("Keep the Teensy A/T switch in MANUAL until telemetry values look correct.")

    try:
        SimConnect, AircraftRequests = load_simconnect()
        sim = SimConnect()
        requests = AircraftRequests(sim, _time=0)
    except Exception as error:
        print(f"Unable to connect to MSFS: {error}", file=sys.stderr)
        return 2

    serial_port = None
    if args.enable_motion:
        try:
            serial_port = open_serial(args.port, args.baud)
            # A Teensy can briefly re-enumerate after a USB serial connection.
            # Send calibration only after that settles, and before any T#### target.
            time.sleep(SERIAL_STARTUP_SETTLE_SECONDS)
            serial_port.write(
                format_layout_command(args.pot_min, args.pot_max, args.detent_fraction).encode("ascii")
            )
            serial_port.write(b"R,half\n")
            print("Sent Teensy calibration layout and Boeing half-travel mode.")
        except Exception as error:
            print(f"Unable to open {args.port}: {error}", file=sys.stderr)
            return 2

    state = SendState()
    valid_samples = 0
    interval_s = 1 / args.update_hz
    try:
        while True:
            try:
                thr1 = requests.get("GENERAL_ENG_THROTTLE_LEVER_POSITION:1")
                thr2 = requests.get("GENERAL_ENG_THROTTLE_LEVER_POSITION:2")
                percent = average_throttle_percent(thr1, thr2)
                if percent is None:
                    valid_samples = 0
                    print("Waiting for valid engine 1 throttle telemetry...")
                else:
                    target = map_percent_to_counts(
                        percent, args.pot_min, args.pot_max, args.detent_fraction
                    )
                    valid_samples += 1
                    if args.enable_motion and valid_samples >= REQUIRED_VALID_SAMPLES:
                        now = time.monotonic()
                        if should_send(target, state, now, args.min_delta, args.min_interval_ms):
                            serial_port.write(format_target_command(target).encode("ascii"))
                            state = SendState(target, now)
                            print(f"MSFS {percent:6.2f}% -> T{target:04d}")
                    else:
                        print(
                            f"MSFS {percent:6.2f}% -> T{target:04d} "
                            f"(sample {valid_samples}/{REQUIRED_VALID_SAMPLES}; not sent)"
                        )
            except Exception as error:
                valid_samples = 0
                print(f"Telemetry error: {error}", file=sys.stderr)
            time.sleep(interval_s)
    except KeyboardInterrupt:
        print("\nStopped; the Teensy firmware will time out the last command.")
    finally:
        if serial_port is not None:
            serial_port.close()
        exit_method = getattr(sim, "exit", None)
        if callable(exit_method):
            exit_method()
    return 0


if __name__ == "__main__":
    raise SystemExit(run())
