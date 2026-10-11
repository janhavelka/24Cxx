#!/usr/bin/env python3
"""Exercise HIL evidence validation against the actual CLI with a host EEPROM model."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import zlib

from hil_runner import build_plan, parse_args, plain, run_plan, run_soak, validate_output


def capture(commands):
    script = "hilmark start\n" + "".join(
        f"{command}\nhilmark case_{index}\n" for index, command in enumerate(commands)
    )
    run = subprocess.run([str(Path(sys.argv[1]).resolve())], input=script, text=True,
                         capture_output=True, timeout=30, check=True)
    text = plain(run.stdout)
    cursor = text.index("CLI marker: token=start") + len("CLI marker: token=start")
    responses = []
    for index in range(len(commands)):
        marker = f"CLI marker: token=case_{index}"
        end = text.index(marker, cursor)
        responses.append(text[cursor:end])
        cursor = end + len(marker)
    return responses


def main():
    commands = [
        "color off", "version", "settings", "health", "read 0 256", "crc 0 256",
        "selftest", "read 255 1", "current 1", "watch 0 16 3 1", "stress 3",
        "rw_suite 32 40 confirm", "xfer_demo 32 40 confirm",
        "stress_mix 32 40 9 confirm", "randbench 32 40 9 confirm",
        "typed_demo 32 confirm", "read 0 256", "health",
    ]
    responses = capture(commands)
    context = {"model": "zetta", "address": 0x50, "capacity": 256}
    baseline = None
    for command, response in zip(commands, responses):
        expected = dict(context)
        if command == "current 1":
            expected.update(current_address=0, payload=baseline[:1])
        if command == "read 0 256" and baseline is not None:
            expected["payload"] = baseline
        evidence = validate_output(command, response, context=expected)
        if command == "read 0 256" and baseline is None:
            baseline = evidence["payload"]
            context["crc"] = zlib.crc32(baseline)
    # Independent full-array comparison also protects bytes outside each scratch window.
    def dump(response):
        return re.findall(r"^[0-9A-F]{5}: (.*?)\s+\|", response, re.M)
    before, after = dump(responses[4]), dump(responses[-2])
    if len(before) != 16 or before != after:
        raise AssertionError("full-array preimage differs after modeled diagnostics")

    # Exercise the complete runner plan, durable backup handling and validators
    # using actual firmware output. Serial framing itself has fault-injection tests.
    with tempfile.TemporaryDirectory() as temporary:
        args = parse_args(["--dry-run", "--model", "zetta", "--address", "0x50",
                           "--out", temporary, "--include-guards"])
        readonly_plan = build_plan(args)
        outputs = iter(capture([step["command"] for step in readonly_plan]))

        class ReadonlySession:
            def command(self, _command, **_kwargs):
                return next(outputs)

        report = {"plan": readonly_plan, "results": []}
        run_plan(ReadonlySession(), args, Path(temporary), report)
        if report["outcome"] != "PASS":
            raise AssertionError([row for row in report["results"] if row.get("outcome") in {"FAIL", "INCONCLUSIVE"}])

    with tempfile.TemporaryDirectory() as temporary:
        args = parse_args(["--dry-run", "--model", "zetta", "--address", "0x50",
                           "--out", temporary, "--allow-write", "--scratch-address", "32",
                           "--scratch-length", "40", "--scratch-rounds", "3", "--soak-seconds", "2", "--include-guards"])
        plan = build_plan(args)
        scripted = [step["command"] for step in plan] + ["read 0 256", "crc 0 256", "job", "health", "heap", "xfer_stats", "console"] * 2
        outputs = iter(zip(scripted, capture(scripted)))

        class RecordedSession:
            def command(self, command, **_kwargs):
                expected_command, output = next(outputs)
                if command != expected_command:
                    raise AssertionError(f"unexpected runner command {command!r}; expected {expected_command!r}")
                return output

        report = {"plan": plan, "results": []}
        run_plan(RecordedSession(), args, Path(temporary), report)
        if report["outcome"] != "PASS":
            raise AssertionError([row for row in report["results"] if row.get("outcome") in {"FAIL", "INCONCLUSIVE"}])
        if (Path(temporary) / "scratch-preimage.bin").read_bytes() != baseline[32:72]:
            raise AssertionError("durable scratch backup differs from known preimage")
        now = [0.0]
        run_soak(RecordedSession(), args, Path(temporary), report, clock=lambda: now[0],
                 sleep=lambda seconds: now.__setitem__(0, now[0] + seconds))
        if report["outcome"] != "PASS" or report["soak"]["cycles"] != 2:
            raise AssertionError(report)
    print(f"[PASS] {len(commands)} real CLI transcripts satisfy HIL evidence contracts")
    print(f"[PASS] complete {len(readonly_plan)}-command read-only and {len(plan)}-command scratch campaigns against modeled CLI")
    print("[PASS] two soak cycles with actual CLI output, checkpoints and optional telemetry")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
