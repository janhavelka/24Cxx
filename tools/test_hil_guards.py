"""Offline negative-command contracts; no serial port or hardware is used."""
from __future__ import annotations

import unittest

from hil_guards import guard_commands, validate_guard


class GuardPlanTests(unittest.TestCase):
    def test_only_rejecting_read_only_commands_are_planned(self):
        allowed = {"_hil_unknown_command_", "help", "read", "current", "crc", "watch", "stress", "page"}
        for capacity in (128, 256, 512, 65536, 131072, 0xFFFFFFFF):
            with self.subTest(capacity=capacity):
                cases = guard_commands(capacity)
                self.assertEqual(len(cases), len({command for command, _ in cases}))
                self.assertLessEqual(len(cases), 32)
                for command, status in cases:
                    self.assertIn(command.split()[0], allowed)
                    self.assertIn(status, {"INVALID_PARAM", "ADDRESS_OUT_OF_RANGE"})
                    self.assertLess(len(command), 128)
                    self.assertNotIn("\n", command)
                    self.assertNotIn("\r", command)
                self.assertIn((f"read {capacity} 1", "ADDRESS_OUT_OF_RANGE"), cases)
                self.assertIn((f"read {capacity - 1} 2", "ADDRESS_OUT_OF_RANGE"), cases)
                self.assertIn((f"watch {capacity} 1 1", "ADDRESS_OUT_OF_RANGE"), cases)
                self.assertIn(("read -1 1", "INVALID_PARAM"), cases)
                self.assertIn(("read 4294967296 1", "INVALID_PARAM"), cases)
                self.assertIn(("read 0 0", "INVALID_PARAM"), cases)
                self.assertIn(("stress 0", "INVALID_PARAM"), cases)

    def test_invalid_capacity_cannot_produce_a_misleading_matrix(self):
        for capacity in (0, -1, 0x100000000, True, 256.0, "256", None):
            with self.subTest(capacity=capacity), self.assertRaises(ValueError):
                guard_commands(capacity)


class GuardEvidenceTests(unittest.TestCase):
    def test_exact_rejection_with_optional_message_prompt_and_color(self):
        for status in ("INVALID_PARAM", "ADDRESS_OUT_OF_RANGE"):
            record = f"[E] {status} detail=0"
            for output in (record, record + "\n", record + ": rejected argument\n",
                           "\n> " + record + "\r\n> ",
                           "\x1b[31m[E]\x1b[0m " + status + " detail=0\n"):
                with self.subTest(status=status, output=output):
                    result = validate_guard("read 256 1", status, output)
                    self.assertEqual(result, dict(command="read 256 1", status=status, detail=0, rejected=True))

    def test_extra_evidence_is_never_hidden_by_expected_error(self):
        error = "[E] INVALID_PARAM detail=0: invalid command/arguments; see help\n"
        extras = (
            "[I] OK detail=0\n",
            "[E] INVALID_PARAM detail=0\n",
            "[E] BUSY detail=0\n",
            "CLI accepted: command=read session=1\n",
            "CLI result: command=read session=1 outcome=SUCCESS status=OK code=0 detail=0\n",
            "Transfer: kind=READ state=SUCCEEDED\n",
            "00000: 01  |.|\n",
            "ESP-ROM:esp32s3\n",
            "unexpected backend output\n",
            "read 0 0\n",
        )
        for extra in extras:
            for output in (error + extra, extra + error):
                with self.subTest(output=output), self.assertRaises(ValueError):
                    validate_guard("read 0 0", "INVALID_PARAM", output)

    def test_missing_wrong_or_malformed_status_fails(self):
        for output in ("", "> ", "[I] OK detail=0\n", "[E] ADDRESS_OUT_OF_RANGE detail=0\n",
                       "[E] INVALID_PARAM\n", "[E] INVALID_PARAM detail=1\n",
                       "[E] INVALID_PARAM detail=-0\n", "[E] INVALID_PARAM detail=00\n",
                       "[E] INVALID_PARAM detail=0 status=OK\n",
                       "[E] INVALID_PARAM detail=0\rhidden\n",
                       "\x1b[2J[E] INVALID_PARAM detail=0\n"):
            with self.subTest(output=output), self.assertRaises(ValueError):
                validate_guard("read 0 0", "INVALID_PARAM", output)

    def test_caller_cannot_request_a_success_or_transport_error(self):
        for status in ("OK", "BUSY", "I2C_NACK", "", "INVALID_PARAM|OK"):
            with self.subTest(status=status), self.assertRaises(ValueError):
                validate_guard("read 0 0", status, f"[E] {status} detail=0\n")
        for command in ("", "read 0 0\nwrite 0 1", "read 0 0\r"):
            with self.subTest(command=command), self.assertRaises(ValueError):
                validate_guard(command, "INVALID_PARAM", "[E] INVALID_PARAM detail=0\n")


if __name__ == "__main__":
    unittest.main()
