#!/usr/bin/env python3
"""Bounded, evidence-checked serial qualification for the shared EEPROM CLI.

The default plan only reads EEPROM. Programming requires an explicit scratch
window and --allow-write. No command is retried and failed work is never
automatically restored: the firmware's retained RAM backup remains available
for an operator, and the runner saves a durable preimage before programming.
"""
from __future__ import annotations

import argparse
from collections import Counter
import datetime as dt
import hashlib
import json
import math
import os
from pathlib import Path
import re
import secrets
import sys
import time
import zlib


ANSI = re.compile(r"\x1b\[[0-?]*[ -/]*[@-~]")
MAX_RESPONSE_BYTES = 8 * 1024 * 1024
MODELS = {
    "zetta": 256, "24c01": 128, "24c02": 256, "24c04": 512,
    "24c08": 1024, "24c16": 2048, "24c32": 4096, "24c64": 8192,
    "24c128": 16384, "24c256": 32768, "24c512": 65536,
    "24lc1025": 131072, "m24m01": 131072,
}
BANK_MASKS = {"24c04": 1, "24c08": 3, "24c16": 7, "24lc1025": 4, "m24m01": 1}
ALIASES = {"dump": "read", "hexdump": "read", "selfcheck": "selftest", "cur": "current"}
SCRATCH = {"rw_suite", "xfer_demo", "stress_mix", "randbench", "typed_demo"}
ACCEPTED = re.compile(r"^CLI accepted: command=(\w+) session=([1-9][0-9]*)(?=\n)", re.M)
TERMINAL = re.compile(
    r"^CLI result: command=(\w+) session=([1-9][0-9]*) "
    r"outcome=(SUCCESS|FAILED|CANCELLED|TIMED_OUT) status=(\w+) code=(-?[0-9]+) detail=(-?[0-9]+)(?=\n)", re.M)
REBOOT = re.compile(r"^(?:Guru Meditation|Brownout|ESP-ROM:|rst:0x|Traceback|Diagnostic CLI; application owns the bus\.|Startup is read-only\.)", re.M)
ERROR = re.compile(r"(?:^|> )\[E\] ([A-Z_]+)\b[^\n]*", re.M)


class EvidenceError(ValueError):
    """The response is complete but fails the requested command's contract."""


class CaptureError(RuntimeError):
    """Serial response provenance/completeness could not be established."""


def plain(output: str) -> str:
    # A prompt can share the first line of an asynchronous completion.
    return re.sub(r"(?m)^(?:> )+", "", ANSI.sub("", output).replace("\r", ""))


def fields(line: str) -> dict[str, str]:
    return dict(re.findall(r"([\w-]+)=([^\s]+)", line))


def number(value: str) -> int:
    try:
        return int(value, 16 if value.lower().startswith("0x") else 10)
    except (ValueError, AttributeError) as error:
        raise EvidenceError(f"invalid integer evidence: {value!r}") from error


def require(condition: bool, reason: str) -> None:
    if not condition:
        raise EvidenceError(reason)


def one_line(output: str, prefix: str) -> str:
    lines = [line for line in output.splitlines() if line.startswith(prefix)]
    require(len(lines) == 1, f"expected exactly one {prefix!r} record")
    return lines[0]


def terminal_evidence(command: str, output: str) -> dict:
    canonical = ALIASES.get(command.split()[0], command.split()[0])
    accepted = list(ACCEPTED.finditer(output))
    terminals = list(TERMINAL.finditer(output))
    require(len(accepted) == 1 and len(terminals) == 1, "missing or duplicate command admission/completion")
    begin, end = accepted[0], terminals[0]
    require(begin[1] == end[1] == canonical and begin[2] == end[2], "command/session identity mismatch")
    require(begin.start() < end.start(), "completion preceded admission")
    require(end[3] == "SUCCESS" and end[4] == "OK" and int(end[5]) == 0,
            f"terminal command failed: {end[3]} {end[4]}")
    return {"command": end[1], "session": int(end[2]), "outcome": end[3], "status": end[4]}


def parse_payload(output: str, address: int, length: int) -> bytes:
    """Require every requested byte, in order, without duplicates or extra rows."""
    result = bytearray()
    rows = 0
    for line in output.splitlines():
        match = re.fullmatch(r"([0-9A-Fa-f]{5,8}):((?: [0-9A-Fa-f]{2}){1,16}) *\|[ -~]*\|", line)
        if match is None:
            require(not re.match(r"^[0-9A-Fa-f]{5,8}:", line), "malformed memory dump row")
            continue
        rows += 1
        require(int(match[1], 16) == address + len(result), "memory payload address gap/duplicate")
        data = bytes.fromhex(match[2])
        require(len(result) + len(data) <= length, "memory payload exceeds requested range")
        result.extend(data)
    require(rows > 0 and len(result) == length, f"incomplete memory payload: {len(result)}/{length} bytes")
    return bytes(result)


def transfer_evidence(output: str, requested: int, kind: str = "READ", address: int | None = None) -> dict:
    line = one_line(output, "Transfer:")
    data = fields(line)
    require(data.get("kind") == kind, "wrong transfer kind")
    require(data.get("state") == "SUCCEEDED" and data.get("status") == "OK", "transfer did not succeed")
    if address is not None:
        require(number(data.get("address", "-1")) == address, "wrong transfer start address")
    require(number(data.get("requested", "-1")) == requested, "wrong requested byte count")
    require(number(data.get("completed", "-1")) == requested, "incomplete transfer")
    require(number(data.get("request-id", "0")) > 0, "missing transfer request identity")
    return data


def validate_output(command: str, output: str, *, context: dict | None = None) -> dict:
    """Validate one complete command response; usable by host transcript tests.

    ``context`` carries previously validated capacity/address/model, and optional
    ``crc`` (expected full-array CRC), ``payload`` (expected read bytes) or
    ``current_address``. Returned ``payload`` is bytes; report writers encode it.
    """
    context = context or {}
    clean = plain(output)
    tokens = command.split()
    require(bool(tokens), "empty command")
    head = ALIASES.get(tokens[0], tokens[0])
    require(REBOOT.search(clean) is None, "firmware reset/exception during command")
    require(ERROR.search(clean) is None, "CLI reported an error")
    evidence: dict = {}
    async_command = head in {"read", "current", "crc", "selftest", "discover", "scan", "stress", "watch"} | SCRATCH
    if async_command:
        evidence["terminal"] = terminal_evidence(command, clean)
    if head == "version":
        require("EEPROM24Cxx diagnostic CLI" in clean, "wrong firmware family")
        evidence["firmware"] = one_line(clean, "EEPROM24Cxx diagnostic CLI")
        evidence["library"] = one_line(clean, "Library:").split(":", 1)[1].strip()
        require(bool(evidence["library"]), "missing library version")
    elif head in {"settings", "model", "geometry"}:
        staged = fields(one_line(clean, "Staged:"))
        active = fields(one_line(clean, "Active:"))
        require({"model", "address", "timeout", "tx", "rx", "offline-threshold"} <= staged.keys(), "incomplete staged settings")
        require({"bound", "model", "address", "capacity", "page", "word-address", "bank-bits", "bank-shift", "write-cycle"} <= active.keys(),
                "incomplete active geometry")
        require(active.get("bound") == "yes", "driver is not bound")
        for source in (staged, active):
            require(source.get("model") == context.get("model", source.get("model")), "model expectation mismatch")
            require(number(source.get("address", "-1")) == context.get("address", number(source.get("address", "-1"))),
                    "address expectation mismatch")
        evidence.update(active)
        evidence["capacity"] = number(active.get("capacity", "0"))
        require(evidence["capacity"] == context.get("capacity", evidence["capacity"]) and evidence["capacity"] > 0,
                "configured capacity mismatch")
        transport = fields(one_line(clean, "Active transport:"))
        require({"timeout", "tx", "rx", "write-payload", "read-chunk", "clock", "ack-polling", "offline-threshold"} <= transport.keys(),
                "incomplete active transport settings")
        require(transport.get("clock") == "yes", "CLI lacks a monotonic clock")
        for source in (staged, transport):
            require(1 <= number(source["timeout"]) <= 1000 and 1 <= number(source["tx"]) <= 130 and
                    1 <= number(source["rx"]) <= 128 and 0 <= number(source["offline-threshold"]) <= 255, "invalid transport limits")
        capacity = evidence["capacity"]
        model = active["model"]
        require(model in MODELS and MODELS[model] == capacity, "unsupported model/capacity")
        page = 256 if model == "m24m01" else 8 if capacity <= 256 else 16 if capacity <= 2048 else 32 if capacity <= 8192 else 64 if capacity <= 32768 else 128
        bank_mask = BANK_MASKS.get(model, 0)
        require(number(active["page"]) == page and number(active["word-address"]) == (1 if capacity <= 2048 else 2) and
                number(active["bank-bits"]) == bin(bank_mask).count("1") and
                number(active["bank-shift"]) == (2 if model == "24lc1025" else 0) and 5 <= number(active["write-cycle"]) <= 1000,
                "configured geometry differs from selected profile")
        require(1 <= number(transport["write-payload"]) <= 128 and 1 <= number(transport["read-chunk"]) <= 128,
                "invalid effective chunk limits")
        require(transport["ack-polling"] in {"yes", "no"}, "missing ACK polling capability")
        evidence["transport"] = transport
        pointer = fields(one_line(clean, "Current pointer:"))
        require(pointer.get("supported") in {"yes", "no"} and pointer.get("known") in {"yes", "no"}, "missing current-read capability/state")
        evidence["current_supported"] = pointer["supported"] == "yes"
    elif head in {"read", "current"}:
        if head == "current":
            address, length = context.get("current_address", 1), number(tokens[1]) if len(tokens) > 1 else 1
        else:
            address, length = number(tokens[1]), number(tokens[2]) if len(tokens) > 2 else 16
        evidence["transfer"] = transfer_evidence(clean, length, "CURRENT_READ" if head == "current" else "READ", address)
        payload = parse_payload(clean, address, length)
        if "payload" in context:
            require(payload == context["payload"], "read payload differs from saved preimage")
        evidence["payload"] = payload
        evidence["sha256"] = hashlib.sha256(payload).hexdigest()
    elif head in {"crc", "selftest"}:
        length = number(tokens[2]) if head == "crc" else context.get("capacity", 0)
        transfer_evidence(clean, length, address=number(tokens[1]) if head == "crc" else 0)
        line = one_line(clean, "CRC32/ISO-HDLC:" if head == "crc" else "Read-only selftest:")
        data = fields(line)
        if head == "selftest":
            require("Read-only selftest: PASS " in line and number(data.get("bytes", "-1")) == length,
                    "selftest did not read the full array")
        else:
            require(number(data.get("address", "-1")) == number(tokens[1]) and number(data.get("length", "-1")) == length,
                    "CRC range differs from request")
        crc_text = data.get("crc", "").rstrip(";")
        require(re.fullmatch(r"0x[0-9A-Fa-f]{8}", crc_text) is not None, "missing complete CRC")
        evidence["crc"] = number(crc_text)
        if "crc" in context:
            require(evidence["crc"] == context["crc"], "CRC differs from independently captured bytes")
    elif head in {"scan", "discover"}:
        line = one_line(clean, "Scan complete:")
        summary = re.fullmatch(r"Scan complete: ([0-9]+) ACK, ([0-9]+) other transport errors\.", line)
        require(summary is not None and int(summary[2]) == 0, "scan has missing evidence or transport errors")
        addresses = [int(value, 16) for value in re.findall(r"^  Found (0x[0-9A-Fa-f]{2})\b", clean, re.M)]
        require(len(set(addresses)) == len(addresses) == int(summary[1]), "scan address count mismatch")
        require(context.get("address", 0x50) in addresses, "expected EEPROM address did not acknowledge")
        evidence["addresses"] = addresses
    elif head in {"stress", "watch"}:
        rounds = number(tokens[1]) if head == "stress" else number(tokens[3])
        label = "Stress" if head == "stress" else "Watch"
        match = re.fullmatch(rf"{label} stopped: ok=([0-9]+) fail=([0-9]+)", one_line(clean, label + " stopped:"))
        require(match is not None and int(match[1]) == rounds and int(match[2]) == 0,
                "repeat stopped early or had failures")
        evidence["rounds"] = rounds
        if head == "watch":
            length, address = number(tokens[2]), number(tokens[1])
            # Each completed watch round emits its own transfer and dump.
            chunks = re.split(r"(?=^Transfer:)", clean, flags=re.M)[1:]
            require(len(chunks) == rounds, "watch transfer count mismatch")
            for chunk in chunks:
                transfer_evidence(chunk, length, address=address)
                payload = parse_payload(chunk, address, length)
                if "array_payload" in context:
                    require(payload == context["array_payload"][address:address + length], "watch sample differs from original array contents")
    elif head in SCRATCH:
        summaries = [line for line in clean.splitlines() if line.startswith("Scratch:")]
        require(len(summaries) == 1, "missing/duplicate final scratch summary")
        summary = fields(summaries[0])
        suite = fields(one_line(clean, "Scratch suite:"))
        require(suite.get("mode") == head and summary.get("stage") == "IDLE", "wrong/incomplete scratch procedure")
        require(summary.get("backup") == "none" and summary.get("restore-required") == "no", "scratch backup still requires restoration")
        require(summary.get("primary") == "OK" and summary.get("restore") == "OK", "primary test or restoration failed")
        require(number(summary.get("address", "-1")) == number(tokens[1]), "scratch address mismatch")
        length = 28 if head == "typed_demo" else number(tokens[2])
        require(number(summary.get("length", "-1")) == length, "scratch length mismatch")
        require(number(suite.get("checks", "0")) > 0 and suite.get("failures") == "0", "scratch checks missing/failed")
        checks = re.findall(r"^Scratch check: name=([\w-]+) result=(PASS|FAIL)$", clean, re.M)
        require(len(checks) == number(suite["checks"]) and all(result == "PASS" for _, result in checks),
                "scratch named check count/failure mismatch")
        operations = fields(one_line(clean, "Scratch operations:"))
        require(set(operations) == {"reads", "writes", "fills", "updates", "verifies", "probes", "current", "current-skipped"},
                "incomplete scratch operation counters")
        require(all(re.fullmatch(r"[0-9]+", value) for value in operations.values()), "invalid scratch operation count")
        expected_checks = Counter({"out-of-range-no-transfer": 1, "backup": 1, "final-window": 1})
        expected_ops = dict(reads=0, writes=0, fills=0, updates=0, verifies=0, probes=0, current=0,
                            **{"current-skipped": 0})
        if head in {"rw_suite", "xfer_demo"}:
            rounds = 8 if head == "rw_suite" else 4
            expected_checks.update(["pattern-write", "pattern-read", "fill-5a", "pattern-verify"])
            expected_ops.update(reads=3, writes=1, fills=1, verifies=1)
            if head == "rw_suite":
                expected_checks.update(["fill-a5", "changed-update", "unchanged-update", "update-read"])
                expected_ops.update(reads=4, fills=2, updates=2)
            else:
                expected_checks.update({name: 7 for name in (
                    "zero-budget", "busy-admission", "stale-owner", "admission-no-transfer",
                    "retained-blocks-admission", "retained-owner", "retained-no-transfer", "exactly-once-result")})
        elif head == "typed_demo":
            rounds = 2
            expected_checks.update({"typed-write": 2, "typed-read": 2, "typed-le-values": 1, "typed-be-signed-values": 1})
            expected_ops.update(reads=4, writes=2)
        elif head == "randbench":
            rounds = number(tokens[3])
            expected_checks.update({"random-write": rounds, "random-read": rounds})
            expected_ops.update(reads=rounds + 2, writes=rounds)
        else:
            rounds = number(tokens[3])
            skipped = number(operations["current-skipped"])
            require(skipped <= rounds, "too many skipped current-read checks")
            expected_checks.update({name: rounds for name in (
                "mixed-byte-write", "mixed-read", "mixed-fill", "mixed-verify", "changed-update",
                "unchanged-update", "presence-probe", "current-pointer-seed")})
            if skipped:
                expected_checks["current-fallback-read"] = skipped
            if rounds != skipped:
                expected_checks["current-read"] = rounds - skipped
            expected_ops.update(reads=2 * rounds + 2 + skipped, writes=rounds, fills=rounds,
                                updates=2 * rounds, verifies=rounds, probes=rounds, current=rounds - skipped,
                                **{"current-skipped": skipped})
        require(summary.get("rounds") == f"{rounds}/{rounds}", "scratch procedure ended before requested rounds")
        require(Counter(name for name, _ in checks) == expected_checks, "missing/duplicate/incorrect named scratch checks")
        require({key: number(value) for key, value in operations.items()} == expected_ops, "scratch operation counts differ from procedure")
        transfers = [fields(line) for line in clean.splitlines() if line.startswith("Transfer:")]
        require(len(transfers) == sum(expected_ops.values()) - expected_ops["probes"] - expected_ops["current-skipped"] + 1,
                "scratch terminal stage count differs from executed operations")
        for result in transfers:
            require(result.get("state") == "SUCCEEDED" and result.get("status") == "OK" and
                    number(result.get("requested", "-1")) == number(result.get("completed", "-2")),
                    "scratch stage did not complete successfully")
            address = number(result.get("address", "-1"))
            requested = number(result.get("requested", "-1"))
            require(number(tokens[1]) <= address and requested > 0 and address + requested <= number(tokens[1]) + length,
                    "scratch stage extends beyond authorized window")
            if result.get("kind") in {"VERIFIED_WRITE", "VERIFIED_FILL", "VERIFIED_UPDATE", "VERIFY"}:
                require(number(result.get("verified", "-1")) == requested, "scratch stage lacks complete verified-byte evidence")
        ids = [number(result.get("request-id", "0")) for result in transfers]
        require(min(ids) > 0 and len(set(ids)) == len(ids), "duplicate/missing scratch stage request identity")
        expected_kinds = Counter({kind: expected_ops[key] for key, kind in (
            ("reads", "READ"), ("writes", "VERIFIED_WRITE"), ("fills", "VERIFIED_FILL"),
            ("updates", "VERIFIED_UPDATE"), ("verifies", "VERIFY"), ("current", "CURRENT_READ")) if expected_ops[key]})
        require(Counter(result.get("kind", "") for result in transfers[:-1]) == expected_kinds,
                "terminal stage kinds disagree with scratch operation counts")
        restored = transfers[-1]
        require(restored.get("kind") == "VERIFIED_WRITE" and number(restored.get("address", "-1")) == number(tokens[1]) and
                number(restored.get("requested", "-1")) == length and number(restored.get("verified", "-1")) == length,
                "restoration did not verify the entire original scratch window")
        require("Original scratch bytes observed by readback after restoration.\n" in clean,
                "missing verified restoration completion")
        evidence.update(summary=summary, suite=suite, operations=operations, checks=checks)
    elif head == "health":
        health = fields(one_line(clean, "Health:"))
        require(all(re.fullmatch(r"[0-9]+", health.get(key, "")) for key in ("consec", "ok", "fail")), "incomplete health counters")
        require(health.get("state") == "READY" and health.get("bound") == "yes" and health.get("consec") == "0",
                "driver is not bound and healthy")
        state = fields(one_line(clean, "EEPROM:"))
        require(all(re.fullmatch(r"[0-9]+", state.get(key, "")) for key in ("write-busy-polls", "ready-at")), "incomplete programming barrier counters")
        require(state.get("transfer-active") == "no" and state.get("result-pending") == "no" and state.get("write-cycle-pending") == "no",
                "driver has outstanding work")
        require(re.match(r"Last: ok=[0-9]+ ms error=[0-9]+ ms [A-Z_]+ detail=-?[0-9]+(?:$|:)", one_line(clean, "Last:")) is not None,
                "missing retained transfer status/timestamps")
        evidence["health"] = health
    elif head == "probe":
        require(re.search(r"^\[I\] OK detail=0(?:\n|:)", clean, re.M) is not None, "probe did not succeed")
    elif head == "color":
        require("Color: off\n" in clean, "color was not disabled")
    elif head == "verbose":
        require("Verbose: off\n" in clean, "verbose was not disabled")
    elif head == "scratch":
        summary = fields(one_line(clean, "Scratch:"))
        require(summary.get("stage") == "IDLE" and summary.get("backup") == "none" and
                summary.get("restore-required") == "no", "existing scratch work/backup requires operator attention")
        evidence["scratch"] = summary
    elif head == "job":
        state = fields(one_line(clean, "CLI job:"))
        require(state.get("active") == "0" and state.get("auto") == "1", "CLI must be idle with automatic owner polling enabled")
        evidence["job"] = state
    elif head == "deadline":
        data = fields(one_line(clean, "Deadline:"))
        require(data.get("ms") == "0", "preexisting CLI deadline changes campaign behavior")
    elif head == "bus":
        bus = fields(one_line(clean, "Bus config:"))
        require(set(bus) == {"sda", "scl", "frequency-hz", "wp-pin"}, "missing application bus metadata")
        require(all(re.fullmatch(r"-?[0-9]+", value) for value in bus.values()), "invalid bus metadata")
        if context.get("bus_hz") is not None:
            require(number(bus["frequency-hz"]) == context["bus_hz"], "bus frequency expectation mismatch")
        evidence["bus"] = bus
    elif head == "stats":
        counters = fields(one_line(clean, "Core jobs consumed:"))
        require(all(re.fullmatch(r"[0-9]+", counters.get(key, "")) for key in ("total", "ok", "fail")), "incomplete CLI counters")
        require(number(counters["total"]) == min(0xFFFFFFFF, number(counters["ok"]) + number(counters["fail"])), "inconsistent CLI counters")
        evidence["stats"] = counters
    elif head == "xfer_stats":
        counters = fields(one_line(clean, "Transfers:"))
        require(all(re.fullmatch(r"[0-9]+", counters.get(key, "")) for key in ("attempts", "read", "write", "probe", "ok", "fail")), "incomplete bus counters")
        require(number(counters["attempts"]) == min(0xFFFFFFFF, sum(number(counters[key]) for key in ("read", "write", "probe"))), "inconsistent transfer-kind counts")
        require(number(counters["attempts"]) == min(0xFFFFFFFF, number(counters["ok"]) + number(counters["fail"])), "inconsistent transport outcomes")
        evidence["transfers"] = counters
    elif head == "stop":
        # Synchronization marker establishes completion; stopping idle work can be silent.
        pass
    else:
        prefixes = {"help": "=== EEPROM24Cxx CLI Help ===", "stats": "Core jobs consumed:",
                    "xfer_stats": "Transfers:", "page": "Page:", "scratch": "Scratch:",
                    "job": "CLI job:", "size": "Active configured capacity:"}
        require(head in prefixes, f"validator has no contract for {head}")
        require(any(line.startswith(prefixes[head]) for line in clean.splitlines()), f"missing {head} response")
    return evidence


def async_command(command: str) -> bool:
    head = ALIASES.get(command.split()[0], command.split()[0])
    return head in {"read", "current", "crc", "selftest", "discover", "scan", "stress", "watch"} | SCRATCH


class Session:
    """One serial owner; unique markers delimit sync responses and finish async ones."""

    def __init__(self, serial, *, timeout: float = 8, transcript=None, clock=time.monotonic,
                 token_factory=lambda: secrets.token_hex(12), max_bytes: int = MAX_RESPONSE_BYTES):
        self.serial = serial
        self.timeout = timeout
        self.transcript = transcript
        self.clock = clock
        self.token_factory = token_factory
        self.max_bytes = max_bytes
        self.framing_lost = False
        self.completed_sessions: set[int] = set()

    def log(self, value: str) -> None:
        if self.transcript is not None:
            self.transcript.write(value)
            self.transcript.flush()

    def write(self, command: str, deadline: float) -> None:
        remaining = deadline - self.clock()
        if remaining <= 0:
            raise CaptureError("command deadline expired before serial write")
        self.serial.write_timeout = min(2.0, remaining)
        payload = (command + "\n").encode("ascii")
        self.log(f"\n>>> {command}\n")
        try:
            written = self.serial.write(payload)
        except Exception as error:
            raise CaptureError(f"serial write failed: {error}") from error
        if written != len(payload):
            raise CaptureError(f"partial serial write: {written}/{len(payload)} bytes")

    def command(self, command: str, *, asynchronous: bool | None = None, timeout: float | None = None, allow_startup: bool = False) -> str:
        if asynchronous is None:
            asynchronous = async_command(command)
        limit = self.timeout if timeout is None else timeout
        deadline = self.clock() + limit
        token = self.token_factory()
        marker = f"CLI marker: token={token}\n"
        self.framing_lost = True
        self.write(command, deadline)
        marked = not asynchronous
        if marked:
            self.write("hilmark " + token, deadline)
        captured = bytearray()
        while self.clock() < deadline:
            self.serial.timeout = min(0.05, max(0.0, deadline - self.clock()))
            try:
                chunk = self.serial.read(min(4096, max(1, int(getattr(self.serial, "in_waiting", 0)))))
            except Exception as error:
                raise CaptureError(f"serial read failed: {error}") from error
            if not chunk:
                continue
            captured.extend(chunk)
            self.log(chunk.decode("utf-8", errors="replace"))
            if len(captured) > self.max_bytes:
                raise CaptureError("response exceeded bounded capture size")
            output = captured.decode("utf-8", errors="replace")
            clean = plain(output)
            if not allow_startup and REBOOT.search(clean):
                raise CaptureError("firmware reset/exception during command")
            if asynchronous and not marked:
                accepted = list(ACCEPTED.finditer(clean))
                completed = list(TERMINAL.finditer(clean))
                if len(accepted) > 1 or len(completed) > 1:
                    raise CaptureError("duplicate async command admission/completion")
                if completed:
                    if not accepted or accepted[0].group(1, 2) != completed[0].group(1, 2):
                        raise CaptureError("unmatched command/session completion")
                    canonical = ALIASES.get(command.split()[0], command.split()[0])
                    if completed[0][1] != canonical:
                        raise CaptureError("completion belongs to a different command")
                    session_id = int(completed[0][2])
                    if session_id in self.completed_sessions:
                        raise CaptureError("CLI reused a completed session identity (possible reset/stale response)")
                    self.completed_sessions.add(session_id)
                    marked = True
                    self.write("hilmark " + token, deadline)
                elif ERROR.search(clean) and not accepted:
                    # Admission failed: synchronize the error without awaiting nonexistent work.
                    marked = True
                    self.write("hilmark " + token, deadline)
            if marked and marker in clean:
                require(clean.count(marker) == 1, "duplicate command synchronization marker")
                before, tail = clean.split(marker, 1)
                if re.fullmatch(r"[>\s]*", tail) is None:
                    raise CaptureError("unexpected output after command synchronization marker")
                self.framing_lost = False
                return before
        raise CaptureError(f"command did not complete before {limit:g}s deadline")


def durable_write(path: Path, payload: bytes) -> None:
    """An existing backup is never overwritten; flush it before any programming."""
    with path.open("xb") as stream:
        stream.write(payload)
        stream.flush()
        os.fsync(stream.fileno())


def json_value(value):
    if isinstance(value, bytes):
        return {"length": len(value), "sha256": hashlib.sha256(value).hexdigest()}
    if isinstance(value, dict):
        return {key: json_value(item) for key, item in value.items()}
    if isinstance(value, (list, tuple)):
        return [json_value(item) for item in value]
    return value


def build_plan(args) -> list[dict]:
    capacity = MODELS[args.model]
    plan = []
    def add(command, purpose, group="safe", writes=False):
        plan.append(dict(command=command, purpose=purpose, group=group, writes=writes))
    for command in ("version", "job", "deadline", "settings", "bus", "scratch", "color off", "verbose off", "help", "geometry", "page 0", "size", "stats", "xfer_stats", "health"):
        add(command, "Firmware/target and cached diagnostics")
    add("discover", "EEPROM-range address acknowledgements")
    add("probe", "Untracked presence check")
    add(f"read 0 {capacity}", "Complete byte capture")
    add(f"crc 0 {capacity}", "Compare firmware CRC with captured bytes")
    add("selftest", "Whole-array read and matching CRC")
    add("read 0 1", "Seed known current pointer")
    add("current 1", "Pure current-pointer read; skip only when adapter reports unsupported")
    add(f"stress {args.rounds}", "Finite read-only stress")
    add(f"watch 0 {min(16, capacity)} 3 1", "Three complete read-only watch samples")
    if args.allow_write:
        address, length = args.scratch_address, args.scratch_length
        add(f"read {address} {length}", "Durable scratch preimage before programming", "backup")
        commands = [f"rw_suite {address} {length} confirm", f"xfer_demo {address} {length} confirm",
                    f"stress_mix {address} {length} {args.scratch_rounds} confirm",
                    f"randbench {address} {length} {args.scratch_rounds} confirm"]
        if length >= 28:
            commands.append(f"typed_demo {address} confirm")
        for command in commands:
            add(command, "Explicit scratch procedure with verified restoration", "scratch", True)
            add(f"read {address} {length}", "Independently compare every byte with durable preimage", "restore-check")
        add(f"read 0 {capacity}", "Check complete array against original capture after scratch procedures", "full-check")
    add("scratch", "Confirm retained backup/restoration state")
    add("health", "Confirm no outstanding work and final tracked health")
    return plan


def positive_float(text: str) -> float:
    value = float(text)
    if not math.isfinite(value) or not 0 < value <= 3600:
        raise argparse.ArgumentTypeError("must be finite and in (0, 3600]")
    return value


def integer(text: str) -> int:
    try:
        if not re.fullmatch(r"(?:0[xX][0-9a-fA-F]+|[0-9]+)", text):
            raise ValueError()
        return number(text)
    except ValueError as error:
        raise argparse.ArgumentTypeError("expected unsigned decimal or 0x hexadecimal integer") from error


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", help="Explicit serial port; required for a live run")
    parser.add_argument("--address", required=True, type=integer, help="Expected strapped base address, 0x50..0x57")
    parser.add_argument("--model", required=True, choices=MODELS, help="Expected configured profile; never autodetected")
    parser.add_argument("--out", required=True, type=Path, help="Artifact parent directory; each run creates a unique child")
    parser.add_argument("--baud", type=integer, default=115200)
    parser.add_argument("--timeout", type=positive_float, default=10.0)
    parser.add_argument("--long-timeout", type=positive_float, default=180.0)
    parser.add_argument("--rounds", type=integer, default=10)
    parser.add_argument("--allow-write", action="store_true", help="Explicitly allow only the selected scratch procedures")
    parser.add_argument("--scratch-address", type=integer)
    parser.add_argument("--scratch-length", type=integer)
    parser.add_argument("--scratch-rounds", type=integer, default=3)
    parser.add_argument("--dry-run", action="store_true", help="Write a NOT_RUN plan without importing/opening serial")
    parser.add_argument("--board", default="unspecified")
    parser.add_argument("--operator", default="unspecified")
    parser.add_argument("--firmware", default="unspecified", help="Operator-recorded flashed build/commit; compared with printed version only by operator")
    parser.add_argument("--chip", default="unspecified", help="Physical manufacturer/part/package marking")
    parser.add_argument("--voltage", default="unspecified", help="Measured EEPROM supply voltage")
    parser.add_argument("--bus-hz", type=integer, help="Recorded actual bus frequency")
    args = parser.parse_args(argv)
    if not args.dry_run and not args.port:
        parser.error("live runs require --port")
    if not 0x50 <= args.address <= 0x57:
        parser.error("--address must be 0x50..0x57")
    if args.address & BANK_MASKS.get(args.model, 0):
        parser.error("--address must be the strapped base with the selected model's bank bits clear")
    if args.bus_hz is not None and not 1 <= args.bus_hz <= 10000000:
        parser.error("--bus-hz must be 1..10000000")
    if not 1 <= args.baud <= 10000000 or not 1 <= args.rounds <= 10000 or not 1 <= args.scratch_rounds <= 100:
        parser.error("baud/round counts are outside supported limits")
    supplied = args.scratch_address is not None or args.scratch_length is not None
    if supplied and not args.allow_write:
        parser.error("scratch arguments require --allow-write")
    if args.allow_write and (args.scratch_address is None or args.scratch_length is None):
        parser.error("--allow-write requires --scratch-address and --scratch-length")
    if args.allow_write and (not 1 <= args.scratch_length <= 256 or
                            args.scratch_address + args.scratch_length > MODELS[args.model]):
        parser.error("scratch window must fit the model and contain 1..256 bytes")
    return args


def run_plan(session: Session, args, directory: Path, report: dict) -> None:
    context = dict(model=args.model, address=args.address, capacity=MODELS[args.model], bus_hz=args.bus_hz)
    original = None
    backup = None
    backup_path = directory / "scratch-preimage.bin"
    for step in report["plan"]:
        command = step["command"]
        if command == "current 1" and not context.get("current_supported", False):
            report["results"].append(dict(**step, outcome="SKIPPED", reason="adapter explicitly reports current-read unsupported"))
            continue
        row = dict(**step)
        report["results"].append(row)
        started = time.monotonic()
        try:
            if step["writes"]:
                require(backup is not None and backup_path.is_file(), "durable backup required before programming")
            validation = dict(context)
            if command == "current 1" and original is not None:
                validation.update(current_address=1, payload=original[1:2])
            if step["group"] == "restore-check":
                validation["payload"] = backup
            if step["group"] == "full-check":
                validation["payload"] = original
            timeout = args.long_timeout if async_command(command) else args.timeout
            output = session.command(command, timeout=timeout)
            evidence = validate_output(command, output, context=validation)
            if command == "settings":
                context["current_supported"] = evidence["current_supported"]
            if command == f"read 0 {context['capacity']}" and original is None:
                original = evidence["payload"]
                context["crc"] = zlib.crc32(original)
                context["array_payload"] = original
                durable_write(directory / "array-before.bin", original)
            if step["group"] == "backup":
                backup = evidence["payload"]
                require(original is not None and backup == original[args.scratch_address:args.scratch_address + args.scratch_length],
                        "scratch changed since initial array capture")
                durable_write(backup_path, backup)
                metadata = dict(model=args.model, address=args.address, offset=args.scratch_address,
                                length=len(backup), sha256=hashlib.sha256(backup).hexdigest())
                durable_write(directory / "scratch-preimage.json", (json.dumps(metadata, indent=2) + "\n").encode())
                report["backup"] = metadata
            row.update(outcome="PASS", evidence=json_value(evidence))
        except EvidenceError as error:
            row.update(outcome="FAIL", reason=str(error))
            report["outcome"] = "FAIL"
            break
        except (CaptureError, OSError, KeyboardInterrupt) as error:
            row.update(outcome="INCONCLUSIVE", reason=str(error) or "operator interrupted")
            report["outcome"] = "INCONCLUSIVE"
            break
        except Exception as error:
            row.update(outcome="INCONCLUSIVE", reason=f"unexpected runner error: {type(error).__name__}: {error}")
            report["outcome"] = "INCONCLUSIVE"
            break
        finally:
            row["elapsed_s"] = round(time.monotonic() - started, 3)
    else:
        report["outcome"] = "PASS"
        return
    for step in report["plan"][len(report["results"]):]:
        report["results"].append(dict(**step, outcome="NOT_RUN", reason="campaign stopped after earlier failure"))
    # Fully framed async responses already carry a terminal result. A preflight
    # failure may concern another owner's work; a partial serial write may leave
    # an unfinished command in the console. Never send cleanup bytes in either
    # case, and never automatically restore a failed programming procedure.
    report["cleanup"] = {"outcome": "NOT_SENT", "reason":
                         "serial framing is uncertain; no bytes sent that could complete a partial command" if
                         getattr(session, "framing_lost", False) else
                         "completed response or preflight rejection; no cancellation of preexisting work or automatic restoration"}


def main(argv=None) -> int:
    args = parse_args(argv)
    timestamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    directory = args.out / ("eeprom-" + timestamp + "-" + secrets.token_hex(4))
    directory.mkdir(parents=True, exist_ok=False)
    report = dict(outcome="NOT_RUN", started_utc=timestamp, model=args.model, address=args.address,
                  port=args.port, baud=args.baud, board=args.board, operator=args.operator,
                  firmware=args.firmware, chip=args.chip, voltage=args.voltage, bus_hz=args.bus_hz,
                  allow_write=args.allow_write, plan=build_plan(args), results=[], serial_executed=False,
                  electrical_evidence="NOT_RUN")
    if args.allow_write and args.scratch_length < 28:
        report["omitted"] = [{"command": "typed_demo", "outcome": "SKIPPED", "reason": "authorized scratch window is smaller than 28 bytes"}]
    serial = None
    try:
        if not args.dry_run:
            # Import only for live runs. No installation, discovery, upload, or
            # deliberate DTR/RTS reset occurs in this tool.
            import serial as pyserial  # type: ignore
            serial = pyserial.Serial(port=None, baudrate=args.baud, timeout=0.05, write_timeout=2)
            serial.dtr = False
            serial.rts = False
            serial.port = args.port
            serial.open()
            with (directory / "transcript.txt").open("x", encoding="utf-8", newline="\n") as transcript:
                session = Session(serial, timeout=args.timeout, transcript=transcript)
                # Unique marker drains startup/stale text without resetting the MCU.
                report["serial_executed"] = True
                session.command("hilmark " + secrets.token_hex(12), asynchronous=False, allow_startup=True)
                run_plan(session, args, directory, report)
    except (ImportError, OSError, CaptureError, EvidenceError, KeyboardInterrupt) as error:
        report.update(outcome="INCONCLUSIVE", reason=str(error) or "operator interrupted")
    except Exception as error:
        report.update(outcome="INCONCLUSIVE", reason=f"unexpected runner error: {type(error).__name__}: {error}")
    finally:
        if serial is not None:
            try:
                serial.close()
            except OSError as error:
                report.update(outcome="INCONCLUSIVE", close_error=str(error))
        (directory / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"{report['outcome']}: {directory}")
    return 0 if report["outcome"] in {"PASS", "NOT_RUN"} else 1 if report["outcome"] == "FAIL" else 2


if __name__ == "__main__":
    raise SystemExit(main())
