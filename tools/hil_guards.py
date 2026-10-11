"""Read-only CLI rejection cases and strict rejection evidence for HIL.

Every case must reject before operation admission or bus access. The runner
also compares transport counters and cached state around the complete matrix;
an error line alone cannot prove that a rejected command was bus-silent.
"""
from __future__ import annotations

import re


_EXPECTED_STATUSES = frozenset({"INVALID_PARAM", "ADDRESS_OUT_OF_RANGE"})
_SGR = re.compile(r"\x1b\[[0-9;]*m")


def guard_commands(capacity: int) -> list[tuple[str, str]]:
    """Return bounded malformed/read-out-of-range commands for this capacity.

    No mutation, lifecycle, target selection or valid memory access is included.
    Capacity comes from the explicitly selected profile, never device discovery.
    """
    if type(capacity) is not int or not 1 <= capacity <= 0xFFFFFFFF:
        raise ValueError("guard capacity must be an unsigned nonzero 32-bit integer")
    invalid = (
        "_hil_unknown_command_",
        "help _hil_unknown_topic_",
        "help tests extra",
        "read",
        "read 0 0",
        "read 0 1 extra",
        "read -1 1",
        "read +1 1",
        "read 1e3 1",
        "read 0xGG 1",
        "read 4294967296 1",
        "read 0x100000000 1",
        "read 0 -1",
        "read 0 4294967296",
        "current 0",
        "crc 0",
        "crc 0 0",
        "watch",
        "watch 0 0",
        "watch 0 1 0",
        "watch 0 1 1 0",
        "stress 0",
        "stress 10001",
        "stress 1 0",
        "stress 1 1 extra",
    )
    outside = (
        f"read {capacity} 1",
        f"read {capacity - 1} 2",
        f"crc {capacity} 1",
        f"watch {capacity} 1 1",
        f"page {capacity}",
    )
    return [(command, "INVALID_PARAM") for command in invalid] + [
        (command, "ADDRESS_OUT_OF_RANGE") for command in outside
    ]


def validate_guard(command: str, expected_status: str, output: str) -> dict:
    """Accept exactly one expected rejection and no other response evidence.

    The caller supplies a completely captured, synchronized response. Prompts,
    blank lines, CRLF and color are allowed; command echoes, transfer/session
    records, startup text and all other extra output fail closed.
    """
    if not command or "\n" in command or "\r" in command:
        raise ValueError("guard command must be one nonempty line")
    if expected_status not in _EXPECTED_STATUSES:
        raise ValueError("unsupported guard rejection status")
    clean = _SGR.sub("", output.replace("\r\n", "\n"))
    clean = re.sub(r"(?m)^> ?", "", clean).strip(" \t\n")
    match = re.fullmatch(r"\[E\] ([A-Z_]+) detail=0(?:: [ -~]+)?", clean)
    if match is None:
        raise ValueError("guard must return exactly one error with detail=0 and no extra output")
    if match[1] != expected_status:
        raise ValueError(f"guard expected {expected_status}, observed {match[1]}")
    return {"command": command, "status": match[1], "detail": 0, "rejected": True}
