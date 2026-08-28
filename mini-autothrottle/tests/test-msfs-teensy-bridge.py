"""Standard-library checks for the safe MSFS bridge mapping and send guard."""

import importlib.util
import pathlib
import sys
import unittest
from unittest.mock import patch


BRIDGE_PATH = pathlib.Path(__file__).parents[1] / "bridge" / "msfs_teensy_bridge.py"
SPEC = importlib.util.spec_from_file_location("msfs_teensy_bridge", BRIDGE_PATH)
bridge = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules[SPEC.name] = bridge
SPEC.loader.exec_module(bridge)


class MappingTests(unittest.TestCase):
    def test_737_calibration(self):
        self.assertEqual(bridge.detent_counts(10, 1013, 0.45), 461)
        self.assertEqual(bridge.map_percent_to_counts(0, 10, 1013, 0.45), 461)
        self.assertEqual(bridge.map_percent_to_counts(50, 10, 1013, 0.45), 737)
        self.assertEqual(bridge.map_percent_to_counts(100, 10, 1013, 0.45), 1013)

    def test_invalid_primary_telemetry_is_not_converted_to_idle(self):
        self.assertIsNone(bridge.average_throttle_percent(None, 50))
        self.assertIsNone(bridge.average_throttle_percent("bad", 50))
        self.assertEqual(bridge.average_throttle_percent(40, None), 40)

    def test_command_is_strict_and_padded(self):
        self.assertEqual(bridge.format_target_command(461), "T0461\n")
        self.assertEqual(bridge.format_layout_command(10, 1013, 0.45), "D,10,1013,461\n")
        with self.assertRaises(ValueError):
            bridge.format_target_command(1024)

    def test_rate_guard(self):
        state = bridge.SendState(461, 10.0)
        self.assertFalse(bridge.should_send(462, state, 10.05, 2, 80))
        self.assertTrue(bridge.should_send(500, state, 10.08, 2, 80))

    def test_motion_flag_requires_a_port_before_connecting_to_msfs(self):
        with patch.object(sys, "argv", ["msfs_teensy_bridge.py", "--enable-motion"]):
            with self.assertRaises(SystemExit) as result:
                bridge.parse_args()
        self.assertEqual(result.exception.code, 2)


if __name__ == "__main__":
    unittest.main()
