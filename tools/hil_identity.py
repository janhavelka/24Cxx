"""Parse reported firmware identity without inferring an EEPROM's identity.

Git expectations compare the printed revision prefix. A seven-character
firmware abbreviation cannot prove which full commit was compiled beyond those
seven characters; the returned identity preserves that observed abbreviation.
"""
from __future__ import annotations

import datetime as dt
import re


_TOKEN = r"[A-Za-z0-9_.+\-]+"
_BANNER = re.compile(
    rf"EEPROM24Cxx diagnostic CLI \| (?P<framework>{_TOKEN}) "
    rf"(?P<framework_version>{_TOKEN}) \| (?P<target>{_TOKEN}) \| built "
    r"(?P<compiled_at>(?:Jan|Feb|Mar|Apr|May|Jun|Jul|Aug|Sep|Oct|Nov|Dec) "
    r"(?: [1-9]|[0-3][0-9]) [0-9]{4} [0-2][0-9]:[0-5][0-9]:[0-5][0-9])"
)
_LIBRARY = re.compile(
    r"(?:Library: )?(?P<version>[0-9]+\.[0-9]+\.[0-9]+) "
    r"\((?P<commit>[0-9a-fA-F]{4,40}|unknown), "
    r"(?P<build_timestamp>unknown|[0-9]{4}-[0-9]{2}-[0-9]{2} "
    r"[0-9]{2}:[0-9]{2}:[0-9]{2}), (?P<tree>clean|dirty|unknown)\)"
)
_EXPECTED_COMMIT = re.compile(r"[0-9a-fA-F]{7,40}")


def _record(value: str) -> str:
    """Accept a single complete record, with or without its line terminator."""
    if not isinstance(value, str):
        raise ValueError("firmware identity record must be text")
    if value.endswith("\r\n"):
        return value[:-2]
    return value.removesuffix("\n")


def parse_identity(banner: str, library: str) -> dict:
    """Parse one plain CLI banner and Library record (or bare VERSION_FULL).

    Unknown build metadata remains unknown. It is evidence of an unavailable
    field, never a successful verification against an explicit expectation.
    """
    banner_match = _BANNER.fullmatch(_record(banner))
    library_match = _LIBRARY.fullmatch(_record(library))
    if banner_match is None or library_match is None:
        raise ValueError("missing or malformed complete firmware identity")
    identity = {**banner_match.groupdict(), **library_match.groupdict()}
    identity["commit"] = identity["commit"].lower()
    # Validate calendar values without depending on the host's date locale.
    month, day, year, clock = identity["compiled_at"].split()
    months = "Jan Feb Mar Apr May Jun Jul Aug Sep Oct Nov Dec".split()
    try:
        hour, minute, second = (int(value) for value in clock.split(":"))
        dt.datetime(int(year), months.index(month) + 1, int(day), hour, minute, second)
        if identity["build_timestamp"] != "unknown":
            dt.datetime.fromisoformat(identity["build_timestamp"])
    except ValueError as error:
        raise ValueError("invalid firmware build timestamp") from error
    return identity


def check_identity(identity: dict, expectations: dict) -> None:
    """Check only explicitly requested fields; mismatches raise ValueError.

    Omitted, None and False expectations are disabled. Commit matching accepts
    either revision's prefix only when both contain at least seven hex digits.
    No local repository or serial port is consulted, and no default verification
    claim is made when the caller supplies no expectations.
    """
    for key in ("version", "framework", "target", "commit"):
        expected = expectations.get(key)
        if expected is None or expected is False:
            continue
        if not isinstance(expected, str) or not expected or expected == "unknown":
            raise ValueError(f"invalid expected firmware {key}")
        actual = identity.get(key)
        if key == "commit":
            if _EXPECTED_COMMIT.fullmatch(expected) is None:
                raise ValueError("expected firmware commit must contain 7..40 hexadecimal digits")
            if not isinstance(actual, str) or _EXPECTED_COMMIT.fullmatch(actual) is None:
                raise ValueError("reported firmware commit is unavailable or shorter than seven hexadecimal digits")
            expected, actual = expected.lower(), actual.lower()
            matched = actual.startswith(expected) or expected.startswith(actual)
        else:
            matched = actual != "unknown" and actual == expected
        if not matched:
            raise ValueError(f"firmware {key} mismatch: reported {actual!r}, expected {expected!r}")
    clean = expectations.get("clean")
    if clean is not None and clean is not False:
        if clean is not True:
            raise ValueError("expected firmware cleanliness must be a boolean")
        if identity.get("tree") != "clean":
            raise ValueError("firmware source tree must report clean")
