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

from hil_artifacts import collect_provenance, render_markdown
from hil_guards import guard_commands, validate_guard
from hil_identity import parse_identity, check_identity


ANSI = re.compile(r"\x1b\[[0-?]*[ -/]*[@-~]")
MAX_RESPONSE_BYTES = 8 * 1024 * 1024
MAX_SOAK_SECONDS = 7 * 24 * 60 * 60
MODELS = {
    "zetta": 256, "24c01": 128, "24c02": 256, "24c04": 512,
    "24c08": 1024, "24c16": 2048, "24c32": 4096, "24c64": 8192,
    "24c128": 16384, "24c256": 32768, "24c512": 65536,
    "24lc1025": 131072, "m24m01": 131072,
}
BANK_MASKS = {"24c04": 1, "24c08": 3, "24c16": 7, "24lc1025": 4, "m24m01": 1}
ALIASES = {"dump": "read", "hexdump": "read", "selfcheck": "selftest", "cur": "current", "detect": "discover"}
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
    result = {}
    detail_owner = None
    for key, value in re.findall(r"([\w-]+)=([^\s]+)", line):
        # Scratch intentionally carries two Status details. Preserve both under
        # their owning status instead of silently keeping only the last value.
        if line.startswith("Scratch:"):
            if key in {"primary", "restore"}:
                detail_owner = key
            elif key == "detail":
                require(detail_owner is not None, "scratch detail has no owning status")
                key = detail_owner + "-detail"
        require(key not in result, f"duplicate evidence field: {key}")
        result[key] = value
    return result


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


def profile_page_size(model: str) -> int:
    capacity = MODELS[model]
    return (256 if model == "m24m01" else 8 if capacity <= 256 else
            16 if capacity <= 2048 else 32 if capacity <= 8192 else
            64 if capacity <= 32768 else 128)


def terminal_evidence(command: str, output: str) -> dict:
    canonical = ALIASES.get(command.split()[0], command.split()[0])
    accepted = list(ACCEPTED.finditer(output))
    terminals = list(TERMINAL.finditer(output))
    require(len(accepted) == 1 and len(terminals) == 1, "missing or duplicate command admission/completion")
    begin, end = accepted[0], terminals[0]
    require(begin[1] == end[1] == canonical and begin[2] == end[2], "command/session identity mismatch")
    require(int(end[2]) <= 0xFFFFFFFF, "command session exceeds uint32")
    require(begin.start() < end.start(), "completion preceded admission")
    require(end[3] == "SUCCESS" and end[4] == "OK" and int(end[5]) == 0 and int(end[6]) == 0,
            f"invalid terminal result: {end[3]} {end[4]} code={end[5]} detail={end[6]}")
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


def validate_transfer_record(data: dict) -> None:
    """Validate successful typed transfer counts and memory-write evidence.

    Accepted data is distinct from completed work and verified contents. Updates
    may skip every write; a verified update still observes the entire requested
    range. Zero-length core operations retain no memory-write effect.
    """
    required = {"kind", "state", "address", "requested", "accepted", "completed", "verified",
                "commit", "last-chunk-commit", "status", "detail", "request-id"}
    require(set(data) == required and all(isinstance(value, str) for value in data.values()),
            "incomplete or unexpected transfer fields")
    writes = {"WRITE", "FILL", "UPDATE", "VERIFIED_WRITE", "VERIFIED_FILL", "VERIFIED_UPDATE"}
    verified_writes = {"VERIFIED_WRITE", "VERIFIED_FILL", "VERIFIED_UPDATE"}
    kind = data["kind"]
    require(kind in writes | {"READ", "CURRENT_READ", "VERIFY"}, "unknown transfer kind")
    require(data["state"] == "SUCCEEDED" and data["status"] == "OK" and data["detail"] == "0",
            "transfer did not succeed with zero error detail")
    counts = {}
    for key in ("requested", "accepted", "completed", "verified", "request-id"):
        require(re.fullmatch(r"[0-9]+", data[key]) is not None, f"invalid transfer {key}")
        counts[key] = number(data[key])
        require(counts[key] <= 0xFFFFFFFF, f"transfer {key} exceeds uint32")
    require(counts["request-id"] > 0, "missing transfer request identity")
    require(re.fullmatch(r"(?:0x[0-9A-Fa-f]+|[0-9]+)", data["address"]) is not None,
            "invalid transfer address")
    address = number(data["address"])
    requested, accepted, verified = (counts[key] for key in ("requested", "accepted", "verified"))
    require(address <= 0xFFFFFFFF and address + requested <= 0xFFFFFFFF, "transfer range exceeds uint32")
    require(counts["completed"] == requested, "incomplete transfer")
    require(accepted <= requested and verified <= requested, "transfer byte counts exceed requested range")

    expected_verified = requested if kind == "VERIFY" or kind in verified_writes else 0
    require(verified == expected_verified, "incorrect verified-byte evidence for transfer kind")
    if kind not in writes:
        require(accepted == 0, "read-only transfer reports accepted write bytes")
    elif kind not in {"UPDATE", "VERIFIED_UPDATE"}:
        require(accepted == requested, "successful write lacks complete acceptance evidence")

    expected_commit = "NOT_APPLICABLE"
    if requested and kind in verified_writes:
        expected_commit = "VERIFIED"
    elif accepted:
        expected_commit = "ACCEPTED"
    expected_last = "ACCEPTED" if accepted else "NOT_APPLICABLE"
    require(data["commit"] == expected_commit, "aggregate write effect disagrees with successful transfer")
    require(data["last-chunk-commit"] == expected_last, "last write effect disagrees with accepted bytes")


def transfer_evidence(output: str, requested: int, kind: str = "READ", address: int | None = None) -> dict:
    line = one_line(output, "Transfer:")
    data = fields(line)
    validate_transfer_record(data)
    require(data["kind"] == kind, "wrong transfer kind")
    if address is not None:
        require(number(data["address"]) == address, "wrong transfer start address")
    require(number(data["requested"]) == requested, "wrong requested byte count")
    validate_stage_records(output, data)
    return data


def validate_stage_records(output: str, transfer: dict) -> None:
    kind = transfer["kind"]
    requested = number(transfer["requested"])
    requirements = {"Write:": kind in {"WRITE", "FILL", "UPDATE", "VERIFIED_WRITE", "VERIFIED_FILL", "VERIFIED_UPDATE"},
                    "Verification:": requested > 0 and (kind == "VERIFY" or kind.startswith("VERIFIED_")),
                    "Update comparison:": requested > 0 and kind in {"UPDATE", "VERIFIED_UPDATE"}}
    for prefix, required in requirements.items():
        lines = [line for line in output.splitlines() if line.startswith(prefix)]
        require(len(lines) == int(required), f"missing or unexpected {prefix} evidence for {kind}")
        if not required:
            continue
        data = fields(lines[0])
        keys = {"status", "detail"} | ({"match"} if prefix == "Verification:" else {"compared", "skipped"} if prefix == "Update comparison:" else set())
        require(set(data) == keys and data["status"] == "OK" and data["detail"] == "0", f"unsuccessful {prefix} evidence")
        if prefix == "Verification:":
            require(data["match"] == "yes", "readback did not match")
        if prefix == "Update comparison:":
            require(number(data["compared"]) == requested and 0 <= number(data["skipped"]) <= requested and
                    number(data["skipped"]) + number(transfer["accepted"]) == requested, "inconsistent update comparison/skip evidence")


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
    if head in {"heap", "console"} and re.fullmatch(r"\s*\[E\] UNSUPPORTED detail=-?[0-9]+(?::[^\n]*)?\n\s*", clean):
        require(head != "heap" or not any(context.get(key) is not None for key in ("min_free_heap", "max_heap_drop")),
                "requested heap qualification requires supported telemetry")
        return {"supported": False, "reason": "application explicitly reports UNSUPPORTED"}
    require(ERROR.search(clean) is None, "CLI reported an error")
    evidence: dict = {}
    async_command = head in {"read", "current", "crc", "selftest", "discover", "scan", "stress", "watch"} | SCRATCH
    if async_command:
        evidence["terminal"] = terminal_evidence(command, clean)
    if head == "version":
        require("EEPROM24Cxx diagnostic CLI" in clean, "wrong firmware family")
        evidence["firmware"] = one_line(clean, "EEPROM24Cxx diagnostic CLI")
        evidence["library"] = one_line(clean, "Library:").split(":", 1)[1].strip()
        try:
            evidence["identity"] = parse_identity(evidence["firmware"], evidence["library"])
            check_identity(evidence["identity"], context.get("expectations", {}))
        except ValueError as error:
            raise EvidenceError(str(error)) from error
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
        page = profile_page_size(model)
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
        detection = fields(one_line(clean, "Detection summary:"))
        expected_count = 112 if head == "scan" else 8
        require(number(detection.get("tested", "0")) == expected_count and
                number(detection.get("ack", "-1")) == len(addresses) and detection.get("errors") == "0" and
                number(detection.get("absent", "-1")) + len(addresses) == expected_count,
                "incomplete or inconsistent address coverage")
        require(all((0x08 if head == "scan" else 0x50) <= address <= (0x77 if head == "scan" else 0x57)
                    for address in addresses), "scan returned an address outside its range")
        base = context.get("address", 0x50)
        mask = BANK_MASKS.get(context.get("model", "zetta"), 0)
        banks = {address for address in range(0x50, 0x58) if address & ~mask == base}
        require(banks <= set(addresses) and
                detection.get("configured-bank-acks") == f"{len(banks)}/{len(banks)}" and
                detection.get("base-ack") == "yes" and
                number(detection.get("candidate-acks", "-1")) == sum(0x50 <= address <= 0x57 for address in addresses),
                "incomplete or inconsistent configured-bank coverage")
        evidence["addresses"] = addresses
    elif head in {"stress", "watch"}:
        rounds = number(tokens[1]) if head == "stress" else number(tokens[3])
        label = "Stress" if head == "stress" else "Watch"
        match = re.fullmatch(rf"{label} stopped: ok=([0-9]+) fail=([0-9]+)", one_line(clean, label + " stopped:"))
        require(match is not None and int(match[1]) == rounds and int(match[2]) == 0,
                "repeat stopped early or had failures")
        evidence["rounds"] = rounds
        summaries = [fields(line) for line in clean.splitlines() if line.startswith("Repeat:") and "state=STOPPED" in line]
        require(len(summaries) == 1, "missing terminal repeat metrics")
        metrics = summaries[0]
        require(metrics.get("mode") == head and number(metrics.get("target", "0")) == rounds and
                number(metrics.get("completed", "0")) == rounds and number(metrics.get("ok", "0")) == rounds and
                metrics.get("fail") == "0", "repeat metrics mismatch")
        capacity = context.get("capacity", 256)
        if head == "watch":
            expected_bytes = number(tokens[2]) * rounds
        else:
            blocks = (capacity + 15) // 16
            expected_bytes = 2 * ((rounds // blocks) * capacity + min(capacity, (rounds % blocks) * 16))
        require(number(metrics.get("bytes", "-1")) == expected_bytes, "missing paired-read or repeat byte evidence")
        require(0 <= number(metrics.get("min-ms", "-1")) <= number(metrics.get("max-ms", "-1")) <= number(metrics.get("elapsed-ms", "-1")),
                "invalid repeat timing evidence")
        evidence["metrics"] = metrics
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
        require(all(-0x80000000 <= number(summary.get(key, "invalid")) <= 0x7FFFFFFF
                    for key in ("primary-detail", "restore-detail")), "missing scratch status details")
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
        stages = re.split(r"(?=^Transfer:)", clean, flags=re.M)[1:]
        transfers = [fields(one_line(stage, "Transfer:")) for stage in stages]
        require(len(transfers) == sum(expected_ops.values()) - expected_ops["probes"] - expected_ops["current-skipped"] + 1,
                "scratch terminal stage count differs from executed operations")
        for stage, result in zip(stages, transfers):
            validate_transfer_record(result)
            validate_stage_records(stage, result)
            address = number(result.get("address", "-1"))
            requested = number(result.get("requested", "-1"))
            require(number(tokens[1]) <= address and requested > 0 and address + requested <= number(tokens[1]) + length,
                    "scratch stage extends beyond authorized window")
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
        if "health_fail" in context:
            require(number(health["fail"]) == context["health_fail"], "tracked health failures changed during campaign")
    elif head == "heap":
        data = fields(one_line(clean, "Heap:"))
        require(set(data) == {"free", "minimum-free", "largest-free-block"} and
                all(re.fullmatch(r"[0-9]+", value) for value in data.values()), "incomplete heap telemetry")
        heap = {key: number(value) for key, value in data.items()}
        require(0 <= heap["minimum-free"] <= heap["free"] <= 0xFFFFFFFF and
                0 <= heap["largest-free-block"] <= heap["free"], "inconsistent heap telemetry")
        if context.get("min_free_heap") is not None:
            require(heap["free"] >= context["min_free_heap"], "free heap below requested minimum")
        if context.get("max_heap_drop") is not None and "initial_free_heap" in context:
            require(context["initial_free_heap"] - heap["free"] <= context["max_heap_drop"],
                    "free heap drop exceeds requested maximum")
        evidence.update(supported=True, heap=heap)
    elif head == "console":
        data = fields(one_line(clean, "Console:"))
        counters = ("baud", "rx-bytes", "rx-overflows", "tx-bytes", "tx-dropped", "write-timeouts", "format-overflows", "queued-bytes", "high-water-bytes")
        require(set(data) == {"backend", "ready", "connected", "rx-loss-observable", *counters}, "incomplete console telemetry")
        require(re.fullmatch(r"[A-Za-z0-9_-]+", data["backend"]) is not None and
                data["ready"] in {"yes", "no"} and data["connected"] in {"-1", "0", "1"} and
                data["rx-loss-observable"] in {"yes", "no"}, "invalid console capability/state")
        require(all(re.fullmatch(r"[0-9]+", data[key]) and number(data[key]) <= 0xFFFFFFFF for key in counters),
                "invalid console counters")
        console = dict(data, **{key: number(data[key]) for key in counters})
        require(console["ready"] == "yes" and console["connected"] != "0", "console is unavailable/disconnected")
        require(console["tx-dropped"] == console["format-overflows"] == console["rx-overflows"] == 0, "console reports lost/truncated input/output")
        require(console["queued-bytes"] <= console["high-water-bytes"] <= 16384, "invalid console queue evidence")
        previous = context.get("console_previous")
        if previous:
            require(all(console[key] >= previous[key] for key in counters if key not in {"baud", "queued-bytes"}), "console counters regressed (possible reset)")
            require(all(console[key] == previous[key] for key in ("backend", "baud", "rx-loss-observable")), "console transport/capabilities changed")
        evidence.update(supported=True, console=console)
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
        previous = context.get("transfers_previous")
        if previous:
            require(all(number(counters[key]) >= number(previous[key]) for key in ("attempts", "read", "write", "probe", "ok", "fail")),
                    "transfer counters regressed during campaign")
            if context.get("read_only"):
                require(number(counters["write"]) == number(previous["write"]), "physical write counter changed during read-only work")
            if context.get("soaking"):
                require(number(counters["fail"]) == number(previous["fail"]), "transport failures changed during soak")
        evidence["transfers"] = counters
    elif head == "stop":
        # Synchronization marker establishes completion; stopping idle work can be silent.
        pass
    elif head == "size":
        record = re.fullmatch(r"Active configured capacity: ([0-9]+) bytes \(not detected\)", one_line(clean, "Active configured capacity:"))
        require(record is not None and number(record[1]) == context.get("capacity", MODELS.get(context.get("model", "zetta"))),
                "configured size does not match expected geometry")
    elif head == "page":
        data = fields(one_line(clean, "Page:"))
        address = number(tokens[1]) if len(tokens) > 1 else 0
        capacity = context.get("capacity", MODELS[context.get("model", "zetta")])
        model = context.get("model", "zetta")
        page = profile_page_size(model)
        pointer_bytes = 1 if capacity <= 2048 else 2
        bank_size = 1 << (8 * pointer_bytes)
        expected = {"address": address, "number": address // page, "offset": address % page,
                    "remaining": min(page - address % page, capacity - address),
                    "bank-remaining": min(bank_size - address % bank_size, capacity - address),
                    "sla": context.get("address", 0x50) | ((address // bank_size) << (2 if model == "24lc1025" else 0)),
                    "pointer": address % bank_size, "pointer-bytes": pointer_bytes}
        require(0 <= address < capacity and set(data) == set(expected) and
                all(number(data[key]) == value for key, value in expected.items()), "page/address encoding differs from selected geometry")
        evidence["page"] = data
    elif head == "help":
        require(one_line(clean, "=== EEPROM24Cxx CLI Help ===") == "=== EEPROM24Cxx CLI Help ===", "missing help header")
        groups = {"detection": ("Detection and binding", {"scan", "probe", "init", "bind", "recover", "model", "addr"}),
                  "tests": ("Explicit scratch tests", {"rw_suite", "xfer_demo", "stress_mix", "randbench", "typed_demo", "scratch"})}
        sections = re.findall(r"^\[([^\]]+)\]$", clean, re.M)
        commands = {line.split()[0] for line in re.findall(r"^  (.+?) +- .+$", clean, re.M)}
        topic = tokens[1] if len(tokens) == 2 else "all"
        if topic == "all":
            require(len(sections) == 8 and len(set(sections)) == 8 and
                    {"help", "read", "write", "job", "console", "stress"} <= commands and
                    all(required <= commands for _, required in groups.values()), "incomplete full help inventory")
        else:
            require(topic in groups, "unsupported help validation topic")
            section, required = groups[topic]
            require(sections == [section] and required <= commands, "help topic inventory mismatch")
        evidence["help"] = dict(topic=topic, sections=sections, commands=sorted(commands))
    else:
        raise EvidenceError(f"validator has no contract for {head}")
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
        self.last_completed_session: int | None = None

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
        pending = bytearray()
        lines = []
        received = 0
        accepted = None
        completed = None
        marker_seen = False
        canonical = ALIASES.get(command.split()[0], command.split()[0])
        while self.clock() < deadline:
            self.serial.timeout = min(0.05, max(0.0, deadline - self.clock()))
            try:
                chunk = self.serial.read(min(4096, max(1, int(getattr(self.serial, "in_waiting", 0)))))
            except Exception as error:
                raise CaptureError(f"serial read failed: {error}") from error
            if not chunk:
                continue
            pending.extend(chunk)
            received += len(chunk)
            self.log(chunk.decode("utf-8", errors="replace"))
            if received > self.max_bytes:
                raise CaptureError("response exceeded bounded capture size")
            # Inspect each complete line once. Repeatedly scanning the complete
            # array dump makes fragmented USB/UART captures quadratic in size.
            complete_lines = pending.split(b"\n")
            pending = complete_lines.pop()
            if len(pending) > 4096 or any(len(line) > 4096 for line in complete_lines):
                raise CaptureError("serial line exceeds bounded CLI record length")
            for raw_line in complete_lines:
                line = plain(raw_line.decode("utf-8", errors="replace") + "\n")
                if marker_seen:
                    if re.fullmatch(r"[>\s]*", line) is None:
                        raise CaptureError("unexpected output after command synchronization marker")
                    continue
                if not allow_startup and REBOOT.search(line):
                    raise CaptureError("firmware reset/exception during command")
                if asynchronous:
                    begin, end = ACCEPTED.match(line), TERMINAL.match(line)
                    if begin:
                        if accepted is not None or completed is not None or begin[1] != canonical:
                            raise CaptureError("duplicate or mismatched async command admission")
                        accepted = begin.group(1, 2)
                    if end:
                        if completed is not None or accepted != end.group(1, 2):
                            raise CaptureError("duplicate or unmatched command/session completion")
                        if end[1] != canonical:
                            raise CaptureError("completion belongs to a different command")
                        session_id = int(end[2])
                        distance = ((session_id - self.last_completed_session) & 0xFFFFFFFF) if self.last_completed_session is not None else 1
                        if session_id > 0xFFFFFFFF or not 0 < distance < 0x80000000:
                            raise CaptureError("CLI reused/regressed a completed session identity (possible reset/stale response)")
                        self.last_completed_session = session_id
                        completed = end.group(1, 2)
                        marked = True
                        self.write("hilmark " + token, deadline)
                    elif not marked and ERROR.search(line) and accepted is None:
                        marked = True
                        self.write("hilmark " + token, deadline)
                if marked and line == marker:
                    marker_seen = True
                else:
                    lines.append(line)
            if marker_seen:
                if re.fullmatch(rb"[>\s]*", pending) is None:
                    raise CaptureError("unexpected output after command synchronization marker")
                self.framing_lost = False
                return "".join(lines)
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
    for command in ("version", "job", "deadline", "settings", "bus", "scratch", "color off", "verbose off", "help", "help detection", "help tests", "geometry", "page 0", "size", "stats", "xfer_stats", "health", "heap", "console"):
        add(command, "Firmware/target and cached diagnostics")
    if args.include_guards:
        for command in ("settings", "health", "xfer_stats"):
            add(command, "Snapshot before read-only rejection checks", "guard-before")
        for command, expected in guard_commands(capacity):
            add(command, "Reject invalid read-only command without I2C or state changes", "guard")
            plan[-1]["expected_error"] = expected
        for command in ("settings", "health", "xfer_stats"):
            add(command, "Assert rejected commands left state and transfers unchanged", "guard-after")
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
    add(f"read 0 {capacity}", "Check complete array against original capture after all procedures", "full-check")
    add("scratch", "Confirm retained backup/restoration state")
    add("health", "Confirm no outstanding work and final tracked health")
    add("heap", "Check final heap and any requested qualification limits")
    add("xfer_stats", "Check final counters and read-only write invariance")
    add("console", "Confirm no console output loss")
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


def soak_duration(text: str) -> float:
    value = float(text)
    if not math.isfinite(value) or not 0 <= value <= MAX_SOAK_SECONDS:
        raise argparse.ArgumentTypeError("duration must be finite, nonnegative and at most seven days")
    return value


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.ArgumentDefaultsHelpFormatter)
    target = parser.add_argument_group("Target and serial session")
    target.add_argument("--port", help="Explicit serial port; required for a live run")
    target.add_argument("--address", required=True, type=integer, help="Expected strapped base address, 0x50..0x57")
    target.add_argument("--model", required=True, choices=MODELS, help="Expected configured profile; never autodetected")
    target.add_argument("--baud", type=integer, default=115200, help="Serial baud rate")
    target.add_argument("--timeout", type=positive_float, default=10.0, help="Synchronous command deadline in seconds")
    target.add_argument("--long-timeout", type=positive_float, default=180.0, help="Whole asynchronous command deadline in seconds")
    target.add_argument("--dtr", type=integer, choices=(0, 1), default=0, help="Use 1 for Arduino ESP32-S2 native CDC; RTS stays 0")
    campaign = parser.add_argument_group("Read-only campaign and soak")
    campaign.add_argument("--rounds", type=integer, default=10, help="Finite paired-read stress rounds")
    campaign.add_argument("--include-guards", action="store_true", help="Test rejected read-only inputs and assert bus/state invariance")
    duration = campaign.add_mutually_exclusive_group()
    duration.add_argument("--soak-seconds", type=soak_duration, default=0.0, help="Read-only soak duration, maximum seven days")
    duration.add_argument("--soak-hours", type=soak_duration, default=0.0, help="Read-only soak duration, maximum 168 hours")
    campaign.add_argument("--soak-interval", type=positive_float, default=1.0, help="Pause between read-only soak cycles in seconds")
    qualification = parser.add_argument_group("Optional qualification requirements")
    qualification.add_argument("--expect-version", help="Required reported library version X.Y.Z")
    qualification.add_argument("--expect-commit", help="Required reported firmware Git SHA, 7..40 hex digits; unknown fails")
    qualification.add_argument("--expect-target", help="Required reported target, e.g. esp32s3")
    qualification.add_argument("--expect-framework", help="Required reported framework, e.g. native-esp-idf")
    qualification.add_argument("--expect-clean", action="store_true", help="Require firmware to report a clean source tree")
    qualification.add_argument("--min-free-heap", type=integer, help="Minimum observed current free heap in bytes; requires telemetry")
    qualification.add_argument("--max-heap-drop", type=integer, help="Maximum free-heap drop from preflight in bytes; requires telemetry")
    scratch = parser.add_argument_group("Explicit programming on disposable scratch memory")
    scratch.add_argument("--allow-write", action="store_true", help="Allow only the selected scratch procedures")
    scratch.add_argument("--scratch-address", type=integer)
    scratch.add_argument("--scratch-length", type=integer)
    scratch.add_argument("--scratch-rounds", type=integer, default=3)
    artifacts = parser.add_argument_group("Reports and automation")
    artifacts.add_argument("--out", required=True, type=Path, help="Artifact parent directory; each run creates a unique child")
    artifacts.add_argument("--dry-run", action="store_true", help="Write a NOT_RUN plan without importing/opening serial")
    artifacts.add_argument("--require-run", action="store_true", help="Exit 2 for NOT_RUN instead of permitting a successful dry run")
    artifacts.add_argument("--quiet", action="store_true", help="Suppress finite-step and periodic soak progress; retain final outcome")
    metadata = parser.add_argument_group("Operator-recorded hardware metadata")
    metadata.add_argument("--board", default="unspecified")
    metadata.add_argument("--operator", default="unspecified")
    metadata.add_argument("--firmware", default="unspecified", help="Operator label only; use --expect-commit for a reported identity check")
    metadata.add_argument("--chip", default="unspecified", help="Physical manufacturer/part/package marking")
    metadata.add_argument("--voltage", default="unspecified", help="Measured EEPROM supply voltage")
    metadata.add_argument("--bus-hz", type=integer, help="Expected configured bus frequency (not an electrical measurement)")
    args = parser.parse_args(argv)
    for value, pattern, label in ((args.expect_version, r"[0-9]+\.[0-9]+\.[0-9]+", "--expect-version"),
                                  (args.expect_commit, r"[0-9a-fA-F]{7,40}", "--expect-commit"),
                                  (args.expect_target, r"[A-Za-z0-9_.+-]+", "--expect-target"),
                                  (args.expect_framework, r"[A-Za-z0-9_.+-]+", "--expect-framework")):
        if value is not None and (value == "unknown" or not re.fullmatch(pattern, value)):
            parser.error(f"invalid {label}")
    if any(value is not None and value > 0xFFFFFFFF for value in (args.min_free_heap, args.max_heap_drop)):
        parser.error("heap limits must be unsigned 32-bit byte counts")
    args.soak_seconds = args.soak_seconds or args.soak_hours * 3600
    if args.soak_seconds > MAX_SOAK_SECONDS:
        parser.error("soak duration cannot exceed seven days")
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


def qualification_context(args) -> dict:
    expectations = {key: getattr(args, "expect_" + key) for key in ("version", "commit", "target", "framework", "clean")
                    if getattr(args, "expect_" + key) not in (None, False)}
    return dict(model=args.model, address=args.address, capacity=MODELS[args.model], bus_hz=args.bus_hz,
                expectations=expectations, min_free_heap=args.min_free_heap, max_heap_drop=args.max_heap_drop,
                read_only=not args.allow_write)


def run_plan(session: Session, args, directory: Path, report: dict, *, progress=None) -> None:
    context = qualification_context(args)
    report["outcome"] = "RUNNING"
    guard_snapshots = {}
    original = None
    backup = None
    backup_path = directory / "scratch-preimage.bin"
    for step in report["plan"]:
        command = step["command"]
        if command == "current 1" and not context.get("current_supported", False):
            report["results"].append(dict(**step, outcome="SKIPPED", reason="adapter explicitly reports current-read unsupported"))
            checkpoint_report(directory, report)
            continue
        row = dict(**step, outcome="RUNNING")
        report["results"].append(row)
        started = time.monotonic()
        try:
            checkpoint_report(directory, report)
            if progress: progress(report)
            if step["writes"]:
                require(backup is not None and backup_path.is_file() and backup_path.read_bytes() == backup,
                        "intact durable backup required before programming")
            validation = dict(context)
            if command == "current 1" and original is not None:
                validation.update(current_address=1, payload=original[1:2])
            if step["group"] == "restore-check":
                validation["payload"] = backup
            if step["group"] == "full-check":
                validation["payload"] = original
            timeout = args.long_timeout if async_command(command) else args.timeout
            output = session.command(command, timeout=timeout)
            if "expected_error" in step:
                try:
                    evidence = validate_guard(command, step["expected_error"], plain(output))
                except ValueError as error:
                    raise EvidenceError(str(error)) from error
            else:
                evidence = validate_output(command, output, context=validation)
            if step["group"] == "guard-before":
                guard_snapshots[command] = plain(output).strip()
            elif step["group"] == "guard-after":
                require(command in guard_snapshots and plain(output).strip() == guard_snapshots[command],
                        "rejected command changed settings, health or physical transfer counters")
            if command == "version":
                report["firmware_identity"] = dict(observed=evidence["identity"], expectations=context["expectations"],
                                                  outcome="MATCHED" if context["expectations"] else "NOT_REQUESTED")
            if command in {"heap", "console"}:
                require(evidence.get("supported") or not context.get(command, False),
                        "previously available telemetry disappeared")
                context[command] = evidence["supported"]
            if command == "health":
                context["health_fail"] = number(evidence["health"]["fail"])
            if command == "console" and evidence.get("supported"):
                context["console_previous"] = evidence["console"]
            if command == "heap" and evidence.get("supported"):
                context.setdefault("initial_free_heap", evidence["heap"]["free"])
            if command == "xfer_stats":
                context["transfers_previous"] = evidence["transfers"]
            if command == "settings":
                context["current_supported"] = evidence["current_supported"]
            if command == f"read 0 {context['capacity']}" and original is None:
                original = evidence["payload"]
                context["crc"] = zlib.crc32(original)
                context["array_payload"] = original
                durable_write(directory / "array-before.bin", original)
                report["baseline"] = dict(filename="array-before.bin", length=len(original), sha256=hashlib.sha256(original).hexdigest())
            if step["group"] == "backup":
                backup = evidence["payload"]
                require(original is not None and backup == original[args.scratch_address:args.scratch_address + args.scratch_length],
                        "scratch changed since initial array capture")
                durable_write(backup_path, backup)
                metadata = dict(model=args.model, address=args.address, offset=args.scratch_address,
                                length=len(backup), sha256=hashlib.sha256(backup).hexdigest())
                durable_write(directory / "scratch-preimage.json", (json.dumps(metadata, indent=2) + "\n").encode())
                report["backup"] = metadata
            row.update(outcome="SKIPPED" if evidence.get("supported") is False else "PASS", evidence=json_value(evidence))
        except EvidenceError as error:
            row.update(outcome="FAIL", reason=str(error))
            report["outcome"] = "FAIL"
        except (CaptureError, OSError, KeyboardInterrupt) as error:
            row.update(outcome="INCONCLUSIVE", reason=str(error) or "operator interrupted")
            report["outcome"] = "INCONCLUSIVE"
        except Exception as error:
            row.update(outcome="INCONCLUSIVE", reason=f"unexpected runner error: {type(error).__name__}: {error}")
            report["outcome"] = "INCONCLUSIVE"
        finally:
            row["elapsed_s"] = round(time.monotonic() - started, 3)
            try:
                checkpoint_report(directory, report)
            except OSError as error:
                capture_failure(report, f"finite-step checkpoint failed: {error}")
                if row["outcome"] != "FAIL":
                    row.update(outcome="INCONCLUSIVE", reason=report["capture_error"])
        if row["outcome"] in {"FAIL", "INCONCLUSIVE"}:
            break
    else:
        report["outcome"] = "PASS"
        checkpoint_report(directory, report)
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
    try:
        checkpoint_report(directory, report)
    except OSError as error:
        capture_failure(report, f"finite campaign checkpoint failed: {error}")


def write_json_report(directory: Path, report: dict) -> None:
    """Replace a complete checkpoint; a host interruption cannot leave half JSON."""
    temporary = directory / "report.json.tmp"
    with temporary.open("w", encoding="utf-8", newline="\n") as stream:
        json.dump(report, stream, indent=2)
        stream.write("\n")
        stream.flush()
        os.fsync(stream.fileno())
    temporary.replace(directory / "report.json")


def checkpoint_report(directory: Path, report: dict) -> None:
    # JSON is authoritative. A missing sidecar must never leave a newly published
    # JSON PASS while only the in-memory result was downgraded.
    summary = directory / "report.md.tmp"
    try:
        with summary.open("w", encoding="utf-8", newline="\n") as stream:
            stream.write(render_markdown(report))
            stream.flush()
            os.fsync(stream.fileno())
        summary.replace(directory / "report.md")
    except OSError as error:
        capture_failure(report, f"Markdown checkpoint failed: {error}")
        write_json_report(directory, report)
        raise
    write_json_report(directory, report)


def capture_failure(report: dict, reason: str) -> None:
    # Artifact/connection failures cannot erase a failure already proved by the device.
    if report.get("outcome") != "FAIL":
        report["outcome"] = "INCONCLUSIVE"
    report.setdefault("capture_error", reason)


class RotatingLog:
    """Keep the current and previous 8 MiB soak segments; never grow RAM/history."""
    def __init__(self, path: Path, limit: int = 8 * 1024 * 1024):
        self.path, self.limit = path, limit
        self.size, self.rotations = 0, 0
        self.stream = path.open("x", encoding="utf-8", newline="\n")

    def write(self, text: str) -> None:
        size = len(text.encode("utf-8"))
        if size > self.limit:
            raise CaptureError("single log record exceeds rotation limit")
        if self.size + size > self.limit:
            self.stream.close()
            self.path.replace(self.path.with_name(self.path.name + ".previous"))
            self.stream = self.path.open("x", encoding="utf-8", newline="\n")
            self.size = 0
            self.rotations += 1
        self.stream.write(text)
        self.size += size

    def flush(self) -> None:
        self.stream.flush()

    def close(self) -> None:
        self.stream.close()


def run_soak(session: Session, args, directory: Path, report: dict, *, clock=time.monotonic, sleep=time.sleep, progress=None) -> None:
    """Read-only, integrity-checked soak following a successful finite campaign.

    A cycle admitted before the duration ends completes under per-command
    deadlines. No new cycle starts afterwards; elapsed/overrun are recorded.
    """
    if not args.soak_seconds or report["outcome"] != "PASS":
        return
    baseline = (directory / "array-before.bin").read_bytes()
    require(len(baseline) == MODELS[args.model], "soak needs a complete saved baseline")
    require(report.get("baseline", {}).get("sha256") == hashlib.sha256(baseline).hexdigest(), "saved baseline no longer matches finite campaign evidence")
    context = qualification_context(args)
    context.update(crc=zlib.crc32(baseline), read_only=True, soaking=True)
    for row in report["results"]:
        evidence = row.get("evidence", {})
        if row["command"] in {"heap", "console"}:
            context[row["command"]] = evidence.get("supported", False)
        if "health" in evidence:
            context["health_fail"] = number(evidence["health"]["fail"])
        if "console" in evidence:
            context["console_previous"] = evidence["console"]
        if "heap" in evidence:
            context.setdefault("initial_free_heap", evidence["heap"]["free"])
        if "transfers" in evidence:
            context["transfers_previous"] = evidence["transfers"]
    started = clock()
    summary = dict(outcome="RUNNING", requested_s=args.soak_seconds, elapsed_s=0.0,
                   cycles=0, commands=0, read_bytes=0, crc_bytes=0,
                   latency_min_s=None, latency_max_s=0.0, latency_total_s=0.0,
                   skipped=[], heap=None, console=context.get("console_previous"),
                   per_command={}, worst_command=None, duration_met=False)
    report.update(outcome="RUNNING", soak=summary)
    checkpoint_report(directory, report)
    if progress: progress(report)
    transcript = events = None
    previous_transcript = getattr(session, "transcript", None)
    checkpoint_at = started
    offset = 0
    try:
        transcript = RotatingLog(directory / "soak-transcript.txt")
        events = RotatingLog(directory / "soak-events.jsonl")
        session.transcript = transcript
        while clock() - started < args.soak_seconds:
            length = min(256, len(baseline) - offset)
            cycle = [f"read {offset} {length}", f"crc 0 {len(baseline)}", "job", "health", "heap", "xfer_stats", "console"]
            for command in cycle:
                row = dict(cycle=summary["cycles"] + 1, command=command)
                before = clock()
                try:
                    expected = dict(context)
                    if command.startswith("read "):
                        expected["payload"] = baseline[offset:offset + length]
                    output = session.command(command, timeout=args.long_timeout if async_command(command) else args.timeout)
                    evidence = validate_output(command, output, context=expected)
                    if evidence.get("supported") is False:
                        require(not context.get(command, False), "previously available telemetry disappeared")
                        if command not in summary["skipped"]:
                            summary["skipped"].append(command)
                        row["outcome"] = "SKIPPED"
                    else:
                        row["outcome"] = "PASS"
                    if command == "health":
                        context["health_fail"] = number(evidence["health"]["fail"])
                    if command == "heap" and evidence.get("supported"):
                        heap = evidence["heap"]
                        context.setdefault("initial_free_heap", heap["free"])
                        if summary["heap"] is None:
                            summary["heap"] = dict(initial_free=context["initial_free_heap"], lowest_free=heap["free"])
                        summary["heap"].update(last_free=heap["free"], minimum_free=heap["minimum-free"],
                                               largest_free_block=heap["largest-free-block"],
                                               lowest_free=min(summary["heap"]["lowest_free"], heap["free"]))
                        context["heap"] = True
                    if command == "console" and evidence.get("supported"):
                        summary["console"] = context["console_previous"] = evidence["console"]
                        context["console"] = True
                    if command == "xfer_stats":
                        context["transfers_previous"] = evidence["transfers"]
                    if command.startswith("read "): summary["read_bytes"] += length
                    if command.startswith("crc "): summary["crc_bytes"] += len(baseline)
                    row["evidence"] = json_value(evidence)
                except EvidenceError as error:
                    row.update(outcome="FAIL", reason=str(error))
                except (Exception, KeyboardInterrupt) as error:
                    row.update(outcome="INCONCLUSIVE", reason=str(error) or "operator interrupted")
                elapsed = max(0.0, clock() - before)
                row["elapsed_s"] = round(elapsed, 6)
                summary["commands"] += 1
                summary["latency_min_s"] = elapsed if summary["latency_min_s"] is None else min(summary["latency_min_s"], elapsed)
                summary["latency_max_s"] = max(summary["latency_max_s"], elapsed)
                summary["latency_total_s"] += elapsed
                # Group by command family, not rotating addresses or cycle IDs.
                timing = summary["per_command"].setdefault(command.split()[0], dict(count=0, min_s=elapsed, max_s=0.0, total_s=0.0))
                timing.update(count=timing["count"] + 1, min_s=min(timing["min_s"], elapsed),
                              max_s=max(timing["max_s"], elapsed), total_s=timing["total_s"] + elapsed)
                timing["mean_s"] = timing["total_s"] / timing["count"]
                if summary["worst_command"] is None or elapsed > summary["worst_command"]["elapsed_s"]:
                    summary["worst_command"] = dict(command=command, cycle=summary["cycles"] + 1, elapsed_s=elapsed)
                if row["outcome"] in {"FAIL", "INCONCLUSIVE"}:
                    summary.update(outcome=row["outcome"], failure=row)
                    report["outcome"] = row["outcome"]
                events.write(json.dumps(row) + "\n")
                events.flush()
                if row["outcome"] in {"FAIL", "INCONCLUSIVE"}:
                    return
            summary["cycles"] += 1
            offset = (offset + length) % len(baseline)
            summary["elapsed_s"] = max(0.0, clock() - started)
            if clock() - checkpoint_at >= 30:
                checkpoint_report(directory, report)
                if progress: progress(report)
                checkpoint_at = clock()
            remaining = args.soak_seconds - (clock() - started)
            if remaining > 0:
                sleep(min(args.soak_interval, remaining))
        if summary["cycles"]:
            summary["outcome"] = report["outcome"] = "PASS"
        else:
            summary.update(outcome="INCONCLUSIVE", reason="no complete soak cycle within requested duration")
            report["outcome"] = "INCONCLUSIVE"
    except (Exception, KeyboardInterrupt) as error:
        reason = str(error) or "operator interrupted"
        capture_failure(report, reason)
        summary.update(outcome=report["outcome"], reason=reason)
    finally:
        summary["elapsed_s"] = max(0.0, clock() - started)
        summary["overrun_s"] = max(0.0, summary["elapsed_s"] - args.soak_seconds)
        summary["duration_met"] = summary["elapsed_s"] >= args.soak_seconds
        summary["transcript_rotations"] = transcript.rotations if transcript else 0
        summary["event_rotations"] = events.rotations if events else 0
        summary["latency_mean_s"] = summary["latency_total_s"] / max(1, summary["commands"])
        session.transcript = previous_transcript
        for stream in (transcript, events):
            if stream is not None:
                try:
                    stream.close()
                except OSError as error:
                    capture_failure(report, f"soak log close failed: {error}")
        summary["outcome"] = report["outcome"]
        try:
            checkpoint_report(directory, report)
        except OSError as error:
            capture_failure(report, f"soak checkpoint failed: {error}")
            summary["outcome"] = report["outcome"]


def print_progress(report: dict) -> None:
    soak = report.get("soak", {})
    if soak.get("outcome") == "RUNNING":
        print(f"Soak: {soak['elapsed_s']:.1f}/{soak['requested_s']:g}s cycles={soak['cycles']} "
              f"commands={soak['commands']} read-bytes={soak['read_bytes']} outcome={report['outcome']}", flush=True)
    elif report.get("results"):
        row = report["results"][-1]
        print(f"Step {len(report['results'])}/{len(report['plan'])}: {row['command']} ({row['outcome']})", flush=True)


def main(argv=None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    args = parse_args(argv)
    started = time.monotonic()
    timestamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    directory = args.out / ("eeprom-" + timestamp + "-" + secrets.token_hex(4))
    directory.mkdir(parents=True, exist_ok=False)
    report = dict(outcome="NOT_RUN", started_utc=timestamp, model=args.model, address=args.address,
                  port=args.port, baud=args.baud, board=args.board, operator=args.operator,
                  firmware=args.firmware, chip=args.chip, voltage=args.voltage, bus_hz=args.bus_hz,
                  allow_write=args.allow_write, plan=build_plan(args), results=[], serial_executed=False,
                  electrical_evidence="NOT_RUN")
    report.update(dtr=args.dtr, soak=dict(outcome="NOT_RUN", requested_s=args.soak_seconds))
    report.update(schema_version=2, finalized=False, provenance=collect_provenance(argv),
                  configuration={key: str(value) if isinstance(value, Path) else value for key, value in vars(args).items()})
    progress = None if args.quiet else print_progress
    report["omitted"] = [{"command": name, "outcome": "NOT_RUN", "reason": "requires a separate physical fixture/procedure"}
                         for name in ("electrical timing and margins", "hardware WP", "power-cut and retention", "USB disconnect/stuck bus")]
    if not args.include_guards:
        report["omitted"].append(dict(command="read-only rejection matrix", outcome="SKIPPED", reason="--include-guards not requested"))
    if not args.allow_write:
        report["omitted"].extend(dict(command=name, outcome="SKIPPED", reason="scratch programming not authorized") for name in sorted(SCRATCH))
    elif args.scratch_length < 28:
        report["omitted"].append(dict(command="typed_demo", outcome="SKIPPED", reason="authorized scratch window is smaller than 28 bytes"))
    serial = None
    try:
        checkpoint_report(directory, report)
        if not args.dry_run:
            # Import only for live runs. No installation, discovery, upload, or
            # deliberate DTR/RTS reset occurs in this tool.
            import serial as pyserial  # type: ignore
            serial = pyserial.Serial(port=None, baudrate=args.baud, timeout=0.05, write_timeout=2)
            serial.dtr = bool(args.dtr)
            serial.rts = False
            serial.port = args.port
            serial.open()
            with (directory / "transcript.txt").open("x", encoding="utf-8", newline="\n") as transcript:
                session = Session(serial, timeout=args.timeout, transcript=transcript)
                # Unique marker drains startup/stale text without resetting the MCU.
                report["serial_executed"] = True
                report["outcome"] = "RUNNING"
                checkpoint_report(directory, report)
                session.command("hilmark " + secrets.token_hex(12), asynchronous=False, allow_startup=True)
                run_plan(session, args, directory, report, progress=progress)
                run_soak(session, args, directory, report, progress=progress)
    except (ImportError, OSError, CaptureError, EvidenceError, KeyboardInterrupt) as error:
        capture_failure(report, str(error) or "operator interrupted")
    except Exception as error:
        capture_failure(report, f"unexpected runner error: {type(error).__name__}: {error}")
    finally:
        if serial is not None:
            try:
                serial.close()
            except OSError as error:
                capture_failure(report, f"serial close failed: {error}")
        report.update(finalized=True, finished_utc=dt.datetime.now(dt.timezone.utc).isoformat(), duration_s=max(0.0, time.monotonic() - started))
        try:
            checkpoint_report(directory, report)
        except OSError as error:
            capture_failure(report, f"report checkpoint failed: {error}")
    print(f"{report['outcome']}: {directory}")
    if "capture_error" in report:
        print(report["capture_error"], file=sys.stderr)
    return 0 if report["outcome"] == "PASS" or (report["outcome"] == "NOT_RUN" and not args.require_run) else 1 if report["outcome"] == "FAIL" else 2


if __name__ == "__main__":
    raise SystemExit(main())
