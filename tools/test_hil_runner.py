"""Offline contract and fake-serial regressions; these are not hardware evidence."""
from __future__ import annotations

import contextlib
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import zlib

import hil_runner as hil


def frame(command, body="", session=7, status="OK", outcome="SUCCESS"):
    return (f"CLI accepted: command={command} session={session}\n" + body +
            f"CLI result: command={command} session={session} outcome={outcome} status={status} code=0 detail=0\n")


def transfer(length, *, kind="READ", address=0):
    return (f"Transfer: kind={kind} state=SUCCEEDED address=0x{address:05X} requested={length} "
            f"accepted=0 completed={length} verified=0 commit=NO_WRITE last-chunk-commit=NO_WRITE "
            "status=OK detail=0 request-id=2147483648\n")


def dump(payload, address=0):
    rows = []
    for offset in range(0, len(payload), 16):
        chunk = payload[offset:offset + 16]
        rows.append(f"{address + offset:05X}:" + "".join(f" {byte:02X}" for byte in chunk) + "  |" + "." * len(chunk) + "|\n")
    return "".join(rows)


def rw_evidence():
    names = ["out-of-range-no-transfer", "backup", "pattern-write", "pattern-read", "fill-5a", "pattern-verify",
             "fill-a5", "changed-update", "unchanged-update", "update-read", "final-window"]
    body = "".join(f"Scratch check: name={name} result=PASS\n" for name in names)
    kinds = ["READ"] * 4 + ["VERIFIED_WRITE"] + ["VERIFIED_FILL"] * 2 + ["VERIFIED_UPDATE"] * 2 + ["VERIFY", "VERIFIED_WRITE"]
    for index, kind in enumerate(kinds):
        row = transfer(8, kind=kind, address=4).replace("request-id=2147483648", f"request-id={index + 1}")
        if kind != "READ":
            row = row.replace("verified=0", "verified=8")
        body += row
    return body + (
        "Scratch: stage=IDLE address=0x00004 length=8 rounds=8/8 backup=none restore-required=no primary=OK detail=0 restore=OK detail=0\n"
        "Scratch suite: mode=rw_suite checks=11 failures=0\n"
        "Scratch operations: reads=4 writes=1 fills=2 updates=2 verifies=1 probes=0 current=0 current-skipped=0\n"
        "Original scratch bytes observed by readback after restoration.\n")


class Clock:
    def __init__(self):
        self.time = 0.0

    def __call__(self):
        return self.time


class FakeSerial:
    def __init__(self, responses, clock, *, chunk_size=4096):
        self.responses = responses
        self.clock = clock
        self.chunk_size = chunk_size
        self.buffer = bytearray()
        self.commands = []
        self.partial_write = False
        self.write_timeout = 0
        self.timeout = 0
        self.marker_tail = ""

    @property
    def in_waiting(self):
        return len(self.buffer)

    def write(self, payload):
        command = payload.decode().strip()
        self.commands.append(command)
        if self.partial_write:
            return len(payload) - 1
        if command.startswith("hilmark "):
            response = f"CLI marker: token={command.split()[1]}\n> " + self.marker_tail
        else:
            response = self.responses.get(command, "")
        self.buffer.extend(response.encode())
        return len(payload)

    def read(self, count):
        self.clock.time += 0.001 if self.buffer else 0.1
        length = min(count, self.chunk_size, len(self.buffer))
        result = bytes(self.buffer[:length])
        del self.buffer[:length]
        return result


class PayloadAndEvidenceTests(unittest.TestCase):
    def test_read_requires_every_byte_and_terminal_identity(self):
        data = bytes(range(32))
        output = frame("read", dump(data, 3) + transfer(32, address=3))
        self.assertEqual(hil.validate_output("read 3 32", output)["payload"], data)
        for bad in (
            output.replace("00013:", "00014:"),
            output.replace(" 1F", ""),
            output.replace("session=7 outcome", "session=8 outcome"),
            output.replace("completed=32", "completed=31"),
            output.replace("requested=32", "requested=16"),
            output + dump(b"\x00", 35),
            output.replace("state=SUCCEEDED", "state=FAILED"),
            output.replace("status=OK code=0", "status=I2C_TIMEOUT code=0"),
        ):
            with self.subTest(output=bad), self.assertRaises(hil.EvidenceError):
                hil.validate_output("read 3 32", bad)

    def test_partial_terminal_line_is_not_complete(self):
        output = frame("read", dump(b"a") + transfer(1))
        with self.assertRaises(hil.EvidenceError):
            hil.validate_output("read 0 1", output.rstrip("\n"))

    def test_admission_ok_prompt_is_not_success(self):
        for output in ("[I] OK detail=0\n> ", "CLI accepted: command=read session=1\n> ",
                       "CLI result: command=read session=1 outcome=SUCCESS status=OK code=0 detail=0\n"):
            with self.subTest(output=output), self.assertRaises(hil.EvidenceError):
                hil.validate_output("read 0 1", output)

    def test_crc_is_compared_to_independent_payload(self):
        crc = zlib.crc32(b"123456789")
        output = frame("crc", f"CRC32/ISO-HDLC: address=0x00000 length=9 crc=0x{crc:08X}\n" + transfer(9))
        self.assertEqual(hil.validate_output("crc 0 9", output, context={"crc": crc})["crc"], crc)
        with self.assertRaises(hil.EvidenceError):
            hil.validate_output("crc 0 9", output, context={"crc": crc ^ 1})

    def test_selftest_requires_full_array(self):
        output = frame("selftest", "Read-only selftest: PASS bytes=9 crc=0xCBF43926; tests access only.\n" + transfer(9))
        hil.validate_output("selftest", output, context={"capacity": 9, "crc": 0xCBF43926})
        with self.assertRaises(hil.EvidenceError):
            hil.validate_output("selftest", output, context={"capacity": 10})

    def test_stress_early_stop_cannot_pass(self):
        output = frame("stress", "Stress stopped: ok=2 fail=0\nRepeat: mode=stress state=STOPPED target=2 completed=2 ok=2 fail=0 bytes=64 elapsed-ms=12 min-ms=1 max-ms=1\n")
        hil.validate_output("stress 2", output)
        with self.assertRaises(hil.EvidenceError):
            hil.validate_output("stress 3", output)

    def test_watch_checks_each_sample_payload(self):
        body = (transfer(2) + dump(b"ab")) * 3 + "Watch stopped: ok=3 fail=0\nRepeat: mode=watch state=STOPPED target=3 completed=3 ok=3 fail=0 bytes=6 elapsed-ms=2 min-ms=0 max-ms=0\n"
        hil.validate_output("watch 0 2 3 1", frame("watch", body))
        with self.assertRaises(hil.EvidenceError):
            hil.validate_output("watch 0 2 3 1", frame("watch", body.replace(dump(b"ab"), "", 1)))
        with self.assertRaises(hil.EvidenceError):
            hil.validate_output("watch 0 2 3 1", frame("watch", body), context={"array_payload": b"ac"})

    def test_scan_requires_expected_address_and_complete_counts(self):
        body = "  Found 0x50 (EEPROM/bank candidate; identity unverified)\nScan complete: 1 ACK, 0 other transport errors.\nDetection summary: tested=8 ack=1 absent=7 errors=0 candidate-acks=1 configured-bank-acks=1/1 base-ack=yes\n"
        hil.validate_output("discover", frame("discover", body), context={"address": 0x50})
        for bad in (body.replace("1 ACK", "2 ACK"), body.replace("0 other", "1 other"), body.replace("0x50", "0x51")):
            with self.assertRaises(hil.EvidenceError):
                hil.validate_output("discover", frame("discover", bad), context={"address": 0x50})
        with self.assertRaises(hil.EvidenceError):
            hil.validate_output("discover", frame("discover", body), context={"address": 0x50, "model": "24lc1025"})
        banked = body.replace("Scan complete:", "  Found 0x54 (EEPROM/bank candidate; identity unverified)\nScan complete:")
        banked = banked.replace("1 ACK", "2 ACK").replace("ack=1 absent=7", "ack=2 absent=6")
        banked = banked.replace("candidate-acks=1", "candidate-acks=2").replace("configured-bank-acks=1/1", "configured-bank-acks=2/2")
        hil.validate_output("discover", frame("discover", banked), context={"address": 0x50, "model": "24lc1025"})

    def test_scratch_needs_primary_restoration_and_complete_named_checks(self):
        body = rw_evidence()
        hil.validate_output("rw_suite 4 8 confirm", frame("rw_suite", body))
        for bad in (body.replace("restore=OK", "restore=VERIFY_MISMATCH"), body.replace("primary=OK", "primary=I2C_TIMEOUT"),
                    body.replace("backup=none", "backup=retained"), body.replace("checks=11", "checks=12"),
                    body.replace("result=PASS", "result=FAIL"), body.replace(" current=0", ""),
                    body.replace("final-window", "unrelated-check"), body.replace("rounds=8/8", "rounds=7/8"),
                    body.replace("updates=2", "updates=1"), body.replace("verified=8", "verified=7"),
                    body.replace("kind=READ", "missing=READ"),
                    body.replace("request-id=2\n", "request-id=1\n"),
                    body.replace("Original scratch bytes observed by readback after restoration.\n", "")):
            with self.subTest(output=bad), self.assertRaises(hil.EvidenceError):
                hil.validate_output("rw_suite 4 8 confirm", frame("rw_suite", bad))

    def test_cached_scratch_rejects_retained_backup(self):
        with self.assertRaises(hil.EvidenceError):
            hil.validate_output("scratch", "Scratch: stage=NONE backup=retained restore-required=yes\n")

    def test_ansi_removed_but_firmware_reset_is_not_hidden(self):
        hil.validate_output("color off", "\x1b[32mColor: off\x1b[0m\r\n> ")
        with self.assertRaises(hil.EvidenceError):
            hil.validate_output("color off", "ESP-ROM: esp32s3\nColor: off\n")

    def test_partial_health_cannot_pass_as_complete_diagnostics(self):
        with self.assertRaises(hil.EvidenceError):
            hil.validate_output("health", "Health: state=READY bound=yes consec=0\n"
                                "EEPROM: transfer-active=no result-pending=no write-cycle-pending=no\n")

    def test_active_cli_job_preflight_is_rejected(self):
        with self.assertRaises(hil.EvidenceError):
            hil.validate_output("job", "CLI job: active=1 command=rw_suite session=2 auto=1\n")

    def test_bus_expectation_is_checked(self):
        text = "Bus config: sda=8 scl=9 frequency-hz=100000 wp-pin=-1\n"
        hil.validate_output("bus", text, context={"bus_hz": 100000})
        with self.assertRaises(hil.EvidenceError):
            hil.validate_output("bus", text, context={"bus_hz": 400000})

    def test_missing_capability_or_geometry_cannot_be_interpreted_as_skip(self):
        text = ("Staged: model=zetta address=0x50 timeout=50 ms tx=128 rx=128 offline-threshold=0\n"
                "Active: bound=yes model=zetta address=0x50 capacity=256 page=8 word-address=1 bank-bits=0 bank-shift=0 write-cycle=5 ms\n"
                "Active transport: timeout=50 ms tx=128 rx=128 write-payload=8 read-chunk=128 clock=yes ack-polling=yes offline-threshold=0\n"
                "Current pointer: supported=no known=no address=0x00000; reset-settling=no\n")
        self.assertFalse(hil.validate_output("settings", text)["current_supported"])
        for bad in (text.replace("supported=no", "supported=unknown"), text.replace("supported=no ", ""),
                    text.replace("page=8", "page=16"), text.replace(" rx=128", ""), text.replace("clock=yes", "clock=no")):
            with self.assertRaises(hil.EvidenceError):
                hil.validate_output("settings", bad)


class SessionTests(unittest.TestCase):
    def session(self, responses, chunk_size=4096):
        clock = Clock()
        serial = FakeSerial(responses, clock, chunk_size=chunk_size)
        session = hil.Session(serial, timeout=1, clock=clock, token_factory=lambda: "TEST")
        return serial, session

    def test_sync_marker_handles_fragmented_prompt_and_ansi(self):
        serial, session = self.session({"color off": "CLI marker: token=STALE\n> \x1b[32mColor: off\x1b[0m\n> "}, 1)
        output = session.command("color off")
        self.assertIn("Color: off\n", output)
        self.assertEqual(serial.commands, ["color off", "hilmark TEST"])

    def test_async_waits_for_complete_result_before_marker(self):
        serial, session = self.session({"read 0 1": "[I] OK detail=0\n> " + frame("read", transfer(1) + dump(b"a"))}, 1)
        output = session.command("read 0 1")
        self.assertEqual(hil.validate_output("read 0 1", output)["payload"], b"a")
        self.assertEqual(serial.commands, ["read 0 1", "hilmark TEST"])

    def test_missing_completion_times_out_without_marker_or_retry(self):
        serial, session = self.session({"read 0 1": "[I] OK detail=0\nCLI accepted: command=read session=1\n> "})
        with self.assertRaises(hil.CaptureError):
            session.command("read 0 1")
        self.assertEqual(serial.commands, ["read 0 1"])

    def test_wrong_async_session_is_capture_error(self):
        output = frame("read", transfer(1) + dump(b"a")).replace("session=7 outcome", "session=8 outcome")
        serial, session = self.session({"read 0 1": output})
        with self.assertRaises(hil.CaptureError):
            session.command("read 0 1")
        self.assertEqual(serial.commands, ["read 0 1"])

    def test_admission_error_synchronizes_without_waiting_for_a_job(self):
        serial, session = self.session({"read 0 1": "[E] NOT_INITIALIZED detail=0\n> "})
        output = session.command("read 0 1")
        self.assertEqual(serial.commands[-1], "hilmark TEST")
        with self.assertRaises(hil.EvidenceError):
            hil.validate_output("read 0 1", output)

    def test_short_write_is_terminal_without_retry(self):
        serial, session = self.session({})
        serial.partial_write = True
        with self.assertRaises(hil.CaptureError):
            session.command("version")
        self.assertEqual(serial.commands, ["version"])
        self.assertTrue(session.framing_lost)

    def test_consistent_foreign_command_is_rejected_before_marker(self):
        serial, session = self.session({"read 0 1": frame("crc")})
        with self.assertRaises(hil.CaptureError):
            session.command("read 0 1")
        self.assertEqual(serial.commands, ["read 0 1"])

    def test_capture_size_is_bounded(self):
        serial, session = self.session({"version": "x" * 100})
        session.max_bytes = 30
        with self.assertRaises(hil.CaptureError):
            session.command("version")

    def test_nonprompt_tail_after_marker_is_not_silently_dropped(self):
        serial, session = self.session({})
        serial.marker_tail = "unexpected tail\n"
        with self.assertRaises(hil.CaptureError):
            session.command("version")

    def test_reused_session_identity_rejects_stale_completion(self):
        _, session = self.session({"stress 1": frame("stress", "Stress stopped: ok=1 fail=0\n")})
        session.command("stress 1")
        with self.assertRaises(hil.CaptureError):
            session.command("stress 1")

    def test_session_wrap_is_allowed_but_backwards_identity_is_rejected(self):
        serial, session = self.session({"stress 1": frame("stress", session=0xFFFFFFFF)})
        session.command("stress 1")
        serial.responses["stress 1"] = frame("stress", session=1)
        session.command("stress 1")
        self.assertEqual(session.last_completed_session, 1)
        serial.responses["stress 1"] = frame("stress", session=0xFFFFFFFF)
        with self.assertRaises(hil.CaptureError):
            session.command("stress 1")

    def test_startup_banner_allowed_only_during_initial_drain(self):
        response = "Diagnostic CLI; application owns the bus. Type 'help' for commands.\n"
        _, session = self.session({"version": response})
        with self.assertRaises(hil.CaptureError):
            session.command("version")
        _, session = self.session({"version": response})
        self.assertIn(response, session.command("version", allow_startup=True))


class PlanAndDurabilityTests(unittest.TestCase):
    def args(self, *extras):
        return hil.parse_args(["--dry-run", "--out", "unused", "--model", "zetta", "--address", "0x50", *extras])

    def test_default_plan_never_programs_or_resets(self):
        plan = hil.build_plan(self.args())
        self.assertTrue(plan)
        self.assertFalse(any(step["writes"] for step in plan))
        self.assertFalse(any(step["command"].split()[0] in hil.SCRATCH | {"wp", "iface_reset", "write", "fill", "restore"} for step in plan))
        self.assertEqual(sum(step["group"] == "full-check" for step in plan), 1)

    def test_write_opt_in_requires_exact_bounded_window(self):
        for extra in (("--allow-write",), ("--scratch-address", "0"),
                      ("--allow-write", "--scratch-address", "0", "--scratch-length", "257"),
                      ("--allow-write", "--scratch-address", "250", "--scratch-length", "8"),
                      ("--timeout", "nan"), ("--rounds", "10001")):
            with self.subTest(args=extra), contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                self.args(*extra)

    def test_live_mode_requires_explicit_port(self):
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            hil.parse_args(["--out", "unused", "--model", "zetta", "--address", "0x50"])

    def test_model_bank_bits_must_be_clear_in_base_address(self):
        for model, address in (("24c04", "0x51"), ("24c08", "0x52"), ("24c16", "0x54"),
                               ("24lc1025", "0x54"), ("m24m01", "0x51")):
            with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                hil.parse_args(["--dry-run", "--out", "unused", "--model", model, "--address", address])

    def test_typed_demo_never_exceeds_authorized_window(self):
        short = hil.build_plan(self.args("--allow-write", "--scratch-address", "0", "--scratch-length", "8"))
        full = hil.build_plan(self.args("--allow-write", "--scratch-address", "0", "--scratch-length", "28"))
        self.assertFalse(any(step["command"].startswith("typed_demo") for step in short))
        self.assertEqual(sum(step["writes"] for step in full), 5)
        first_write = next(index for index, step in enumerate(full) if step["writes"])
        self.assertEqual(full[first_write - 1]["group"], "backup")
        self.assertEqual(sum(step["group"] == "restore-check" for step in full), 5)

    def test_durable_backup_cannot_overwrite_an_existing_file(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "backup.bin"
            hil.durable_write(path, b"original")
            with self.assertRaises(FileExistsError):
                hil.durable_write(path, b"replacement")
            self.assertEqual(path.read_bytes(), b"original")

    def test_backup_io_failure_prevents_programming_and_never_restores(self):
        args = self.args("--allow-write", "--scratch-address", "0", "--scratch-length", "8")
        report = {"plan": [
            {"command": "read 0 256", "group": "safe", "writes": False},
            {"command": "read 0 8", "group": "backup", "writes": False},
            {"command": "rw_suite 0 8 confirm", "group": "scratch", "writes": True}], "results": []}
        class Fixture:
            commands = []
            def command(self, command, **_kwargs):
                self.commands.append(command)
                if command == "stop":
                    return ""
                length = int(command.split()[2])
                return frame("read", transfer(length) + dump(bytes(length)))
        fixture = Fixture()
        with tempfile.TemporaryDirectory() as temporary, patch.object(hil, "durable_write", side_effect=[None, OSError("disk full")]):
            hil.run_plan(fixture, args, Path(temporary), report)
        self.assertEqual(report["outcome"], "INCONCLUSIVE")
        self.assertEqual(fixture.commands, ["read 0 256", "read 0 8"])

    def test_preflight_failure_never_cancels_foreign_work(self):
        class Fixture:
            commands = []
            def command(self, command, **_kwargs):
                self.commands.append(command)
                return "CLI job: active=1 command=rw_suite session=2 auto=1\n"
        fixture = Fixture()
        report = {"plan": [{"command": "job", "group": "safe", "writes": False}], "results": []}
        with tempfile.TemporaryDirectory() as temporary:
            hil.run_plan(fixture, self.args(), Path(temporary), report)
        self.assertEqual(fixture.commands, ["job"])
        self.assertEqual(report["outcome"], "FAIL")
        self.assertEqual(report["cleanup"]["outcome"], "NOT_SENT")

    def test_optional_telemetry_cannot_disappear_after_preflight(self):
        class Fixture:
            calls = 0
            def command(self, command, **_kwargs):
                self.calls += 1
                return console_record() if self.calls == 1 else "[E] UNSUPPORTED detail=0: no telemetry\n"
        report = {"plan": [{"command": "console", "group": "safe", "writes": False}] * 2, "results": []}
        with tempfile.TemporaryDirectory() as temporary:
            hil.run_plan(Fixture(), self.args(), Path(temporary), report)
        self.assertEqual(report["outcome"], "FAIL")
        self.assertIn("telemetry disappeared", report["results"][-1]["reason"])

    def test_serial_close_error_preserves_proven_failure(self):
        class BrokenSerial:
            def __init__(self, **_kwargs): pass
            def open(self): pass
            def close(self): raise OSError("close failed")
        class SerialModule:
            Serial = BrokenSerial
        def failed_plan(_session, _args, _directory, report):
            report["outcome"] = "FAIL"
        with tempfile.TemporaryDirectory() as temporary, patch.dict("sys.modules", {"serial": SerialModule}), \
                patch.object(hil.Session, "command", return_value=""), patch.object(hil, "run_plan", side_effect=failed_plan), \
                contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            result = hil.main(["--port", "fake", "--out", temporary, "--model", "zetta", "--address", "0x50"])
            report = json.loads(next(Path(temporary).glob("*/report.json")).read_text())
        self.assertEqual(result, 1)
        self.assertEqual(report["outcome"], "FAIL")
        self.assertIn("serial close failed", report["capture_error"])

    def test_unexpected_parser_error_is_inconclusive_not_not_run(self):
        class Fixture:
            def command(self, command, **_kwargs):
                return "unexpected fixture response"
        report = {"outcome": "NOT_RUN", "plan": [{"command": "job", "group": "safe", "writes": False}], "results": []}
        with tempfile.TemporaryDirectory() as temporary, patch.object(hil, "validate_output", side_effect=RuntimeError("parser defect")):
            hil.run_plan(Fixture(), self.args(), Path(temporary), report)
        self.assertEqual(report["outcome"], "INCONCLUSIVE")

    def test_cleanup_error_preserves_original_capture_reason(self):
        report = {"outcome": "RUNNING"}
        hil.capture_failure(report, "serial disconnected")
        hil.capture_failure(report, "close failed")
        self.assertEqual(report, {"outcome": "INCONCLUSIVE", "capture_error": "serial disconnected"})

    def test_dry_run_creates_not_run_evidence_without_serial_import(self):
        with tempfile.TemporaryDirectory() as temporary, contextlib.redirect_stdout(io.StringIO()):
            result = hil.main(["--dry-run", "--out", temporary, "--model", "zetta", "--address", "0x50"])
            self.assertEqual(result, 0)
            reports = list(Path(temporary).glob("*/report.json"))
            self.assertEqual(len(reports), 1)
            import json
            report = json.loads(reports[0].read_text())
            self.assertEqual(report["outcome"], "NOT_RUN")
            self.assertFalse(report["serial_executed"])
            self.assertEqual(report["electrical_evidence"], "NOT_RUN")


HEALTH = ("Health: state=READY bound=yes consec=0 ok=1 fail=0\n"
          "EEPROM: transfer-active=no result-pending=no write-cycle-pending=no write-busy-polls=0 ready-at=0\n"
          "Last: ok=1 ms error=0 ms OK detail=0\n")


def console_record(count=100):
    return (f"Console: backend=test ready=yes connected=1 baud=115200 rx-bytes={count} rx-overflows=0 rx-loss-observable=yes "
            f"tx-bytes={count} tx-dropped=0 write-timeouts=0 format-overflows=0 queued-bytes=100 high-water-bytes=1024\n")


class SoakTests(unittest.TestCase):
    def args(self, *extras):
        return hil.parse_args(["--dry-run", "--out", "unused", "--model", "zetta", "--address", "0x50", *extras])

    def run_fixture(self, seconds=4, fault=None):
        timer = Clock()
        original = bytes(range(256))
        class Fixture:
            def __init__(self):
                self.commands = []
                self.transcript = None
            def command(self, command, **_kwargs):
                self.commands.append(command)
                timer.time += 0.01
                if fault:
                    injected = fault(command, len(self.commands))
                    if injected is not None:
                        return injected
                tokens = command.split()
                if tokens[0] == "read":
                    address, length = int(tokens[1]), int(tokens[2])
                    return frame("read", transfer(length, address=address) + dump(original[address:address + length], address))
                if tokens[0] == "crc":
                    return frame("crc", transfer(256) + f"CRC32/ISO-HDLC: address=0x00000 length=256 crc=0x{zlib.crc32(original):08X}\n")
                if command == "health": return HEALTH
                if command == "job": return "CLI job: active=0 command=crc session=1 auto=1\n"
                if command == "heap": return "Heap: free=10000 minimum-free=9000 largest-free-block=8000 bytes\n"
                if command == "console": return console_record(100 + len(self.commands))
                raise AssertionError(command)
        fixture = Fixture()
        report = {"outcome": "PASS", "results": []}
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            (directory / "array-before.bin").write_bytes(original)
            hil.run_soak(fixture, self.args("--soak-seconds", str(seconds)), directory, report,
                         clock=timer, sleep=lambda duration: setattr(timer, "time", timer.time + duration))
            stored = json.loads((directory / "report.json").read_text())
            events = [json.loads(line) for line in (directory / "soak-events.jsonl").read_text().splitlines()]
            self.assertEqual(stored["outcome"], report["outcome"])
        return report, fixture.commands, events

    def test_long_soak_aggregates_without_growing_report_history(self):
        report, commands, events = self.run_fixture(1000)
        self.assertEqual(report["outcome"], "PASS")
        self.assertGreater(report["soak"]["cycles"], 900)
        self.assertEqual(report["soak"]["commands"], len(commands))
        self.assertEqual(len(events), len(commands))
        self.assertEqual(report["results"], [])
        self.assertLess(len(json.dumps(report)), 2000)
        self.assertFalse(set(c.split()[0] for c in commands) & (hil.SCRATCH | {"write", "recover", "restore", "stop"}))

    def test_corruption_stops_without_retry_recovery_or_more_commands(self):
        report, commands, _ = self.run_fixture(fault=lambda command, n: frame("crc", transfer(256) + "CRC32/ISO-HDLC: address=0x00000 length=256 crc=0x00000000\n") if command.startswith("crc") else None)
        self.assertEqual(report["outcome"], "FAIL")
        self.assertEqual(commands, ["read 0 256", "crc 0 256"])

    def test_failed_event_write_does_not_erase_proven_corruption(self):
        original_write = hil.RotatingLog.write
        def broken_write(log, text):
            if '"outcome": "FAIL"' in text:
                raise OSError("disk full")
            return original_write(log, text)
        with patch.object(hil.RotatingLog, "write", broken_write):
            report, commands, _ = self.run_fixture(fault=lambda command, n: frame("crc", transfer(256) + "CRC32/ISO-HDLC: address=0x00000 length=256 crc=0x00000000\n") if command.startswith("crc") else None)
        self.assertEqual(report["outcome"], "FAIL")
        self.assertEqual(report["soak"]["failure"]["outcome"], "FAIL")
        self.assertEqual(report["capture_error"], "disk full")
        self.assertEqual(len(commands), 2)

    def test_disconnect_is_inconclusive_and_never_reconnects(self):
        def disconnected(command, n):
            if n == 2: raise hil.CaptureError("device disappeared")
        report, commands, _ = self.run_fixture(fault=disconnected)
        self.assertEqual(report["outcome"], "INCONCLUSIVE")
        self.assertEqual(len(commands), 2)

    def test_console_loss_and_health_errors_fail(self):
        for target, response in (("console", console_record().replace("tx-dropped=0", "tx-dropped=1")),
                                 ("health", HEALTH.replace("consec=0", "consec=1"))):
            report, commands, _ = self.run_fixture(fault=lambda command, n: response if command == target else None)
            self.assertEqual(report["outcome"], "FAIL")
            self.assertEqual(commands[-1], target)

    def test_explicit_optional_telemetry_skip_is_recorded(self):
        report, _, _ = self.run_fixture(fault=lambda command, n: "[E] UNSUPPORTED detail=0: no telemetry\n" if command in {"heap", "console"} else None)
        self.assertEqual(report["outcome"], "PASS")
        self.assertEqual(report["soak"]["skipped"], ["heap", "console"])

    def test_rotation_bounds_disk_and_retains_newest_evidence(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "events"
            log = hil.RotatingLog(path, limit=10)
            for index in range(20): log.write(f"row {index}\n")
            log.close()
            files = list(Path(temporary).iterdir())
            self.assertEqual(len(files), 2)
            self.assertLessEqual(sum(f.stat().st_size for f in files), 20)
            self.assertIn("row 19", path.read_text())

    def test_soak_arguments_and_control_lines_are_explicit(self):
        self.assertEqual(self.args("--soak-hours", "2").soak_seconds, 7200)
        self.assertEqual(self.args("--dtr", "1").dtr, 1)
        for extras in (("--soak-hours", "169"), ("--soak-seconds", "nan"), ("--soak-interval", "0"),
                       ("--soak-hours", "1", "--soak-seconds", "1"), ("--dtr", "2")):
            with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit): self.args(*extras)

    def test_console_partial_or_unmeasurable_fields_are_not_invented(self):
        evidence = hil.validate_output("console", console_record().replace("rx-loss-observable=yes", "rx-loss-observable=no"))
        self.assertEqual(evidence["console"]["rx-loss-observable"], "no")
        for field in ("ready=yes", "rx-bytes=100", "rx-loss-observable=yes", "high-water-bytes=1024"):
            with self.assertRaises(hil.EvidenceError): hil.validate_output("console", console_record().replace(field, ""))


if __name__ == "__main__":
    unittest.main()
