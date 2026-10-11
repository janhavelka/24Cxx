#!/usr/bin/env python3
"""Exercise HIL evidence validation against the actual CLI with a host EEPROM model."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import zlib

from hil_runner import build_plan, parse_args, plain, run_plan, validate_output


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
                           "--out", temporary, "--allow-write", "--scratch-address", "32",
                           "--scratch-length", "40", "--scratch-rounds", "3"])
        plan = build_plan(args)
        outputs = iter(zip((step["command"] for step in plan),
                           capture([step["command"] for step in plan])))

        class RecordedSession:
            def command(self, command, **_kwargs):
                expected_command, output = next(outputs)
                if command != expected_command:
                    raise AssertionError(f"unexpected runner command {command!r}; expected {expected_command!r}")
                return output

        report = {"plan": plan, "results": []}
        run_plan(RecordedSession(), args, Path(temporary), report)
        if report["outcome"] != "PASS":
            raise AssertionError(report)
        if (Path(temporary) / "scratch-preimage.bin").read_bytes() != baseline[32:72]:
            raise AssertionError("durable scratch backup differs from known preimage")
    print(f"[PASS] {len(commands)} real CLI transcripts satisfy HIL evidence contracts")
    print(f"[PASS] complete {len(plan)}-command runner campaign against modeled CLI")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
