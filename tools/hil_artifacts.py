"""Host provenance and readable reports for the EEPROM hardware runner.

These helpers never import serial, open a port, or identify the flashed image.
"""
from __future__ import annotations

from collections import Counter
import hashlib
import html
from importlib import metadata
import json
from pathlib import Path
import platform
import re
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
GIT_TIMEOUT_SECONDS = 3
MAX_GIT_TEXT = 64 * 1024


def _git_text(*arguments: str) -> str | None:
    try:
        result = subprocess.run(
            ["git", *arguments], cwd=ROOT, capture_output=True, text=True,
            encoding="utf-8", errors="replace", check=False,
            timeout=GIT_TIMEOUT_SECONDS,
        )
    except (OSError, subprocess.TimeoutExpired):
        return None
    if result.returncode or len(result.stdout) > MAX_GIT_TEXT:
        return None
    return result.stdout.strip()


def collect_provenance(argv: list[str]) -> dict:
    """Record the host invocation without claiming it matches flashed firmware."""
    commit = _git_text("rev-parse", "HEAD")
    status = _git_text("status", "--porcelain=v1", "--untracked-files=normal")
    if commit is None or re.fullmatch(r"(?:[0-9a-fA-F]{40}|[0-9a-fA-F]{64})", commit) is None:
        commit = "unknown"
    try:
        serial_version = metadata.version("pyserial")
    except metadata.PackageNotFoundError:
        serial_version = "not installed"
    except (OSError, ValueError):
        serial_version = "unavailable"
    sources = {}
    names = ["hil_runner.py", "hil_artifacts.py"]
    names.extend(name for name in ("hil_identity.py", "hil_guards.py") if (ROOT / "tools" / name).is_file())
    for name in names:
        try:
            sources[name] = hashlib.sha256((ROOT / "tools" / name).read_bytes()).hexdigest()
        except OSError:
            sources[name] = "unavailable"
    return {
        "scope": "Host checkout and runner only; this does not identify the flashed firmware.",
        "argv": list(argv),
        "python": {"version": platform.python_version(), "executable": sys.executable},
        "os": platform.platform(),
        "pyserial_version": serial_version,
        "repository": {
            "commit": commit,
            "worktree": "unknown" if status is None else "dirty" if status else "clean",
        },
        "sources": sources,
    }


def _text(value) -> str:
    """Keep operator/firmware strings inside one plain Markdown table cell."""
    if value is None or value == "":
        return "not recorded"
    if isinstance(value, (dict, list, tuple)):
        value = json.dumps(value, ensure_ascii=False, default=str)
    value = html.escape(str(value), quote=False)
    value = value.translate({ord(character): f"&#{ord(character)};" for character in "\\|`[]()*_#!{}"})
    return value.replace("\r\n", "\n").replace("\r", "\n").replace("\n", "<br>")


def _counts(rows: list[dict]) -> str:
    counts = Counter(row.get("outcome", "UNRECORDED") for row in rows)
    return ", ".join(f"{count} {_text(outcome)}" for outcome, count in sorted(counts.items())) or "none"


def render_markdown(report: dict) -> str:
    """Render a complete or interrupted report without inferring missing evidence."""
    rows = report.get("results", [])
    soak = report.get("soak", {})
    provenance = report.get("provenance", {})
    address = report.get("address")
    if isinstance(address, int):
        address = f"0x{address:02X}"
    lines = [
        "# EEPROM24Cxx HIL report", "",
        f"Overall outcome: **{_text(report.get('outcome'))}**.", "",
        "| Run | Recorded value |", "| --- | --- |",
    ]
    values = (
        ("Started UTC", report.get("started_utc")),
        ("Finished UTC", report.get("finished_utc")),
        ("Elapsed seconds", report.get("duration_s")),
        ("Model / base address", f"{report.get('model', 'not recorded')} / {address or 'not recorded'}"),
        ("Board", report.get("board")), ("Chip marking", report.get("chip")),
        ("Operator", report.get("operator")),
        ("Supply voltage", report.get("voltage")),
        ("Recorded bus frequency Hz", report.get("bus_hz")),
        ("Serial port / baud", f"{report.get('port', 'not recorded')} / {report.get('baud', 'not recorded')}"),
        ("Serial commands attempted", report.get("serial_executed", False)),
        ("Finalized after cleanup", report.get("finalized", False)),
        ("Scratch writes authorized", report.get("allow_write", False)),
        ("Flashed firmware label (operator supplied)", report.get("firmware")),
        ("Electrical evidence", report.get("electrical_evidence", "NOT_RUN")),
    )
    lines.extend(f"| {label} | {_text(value)} |" for label, value in values)
    if report.get("firmware_identity"):
        lines.extend(["", f"Firmware identity comparison: {_text(report['firmware_identity'])}."])
    lines.extend(["", "## Finite campaign", "", f"Recorded results: {_counts(rows)}.", ""])
    if rows:
        lines.extend(["| Command | Phase | Outcome | Seconds | Reason |", "| --- | --- | --- | ---: | --- |"])
        for row in rows:
            lines.append("| " + " | ".join(_text(row.get(key)) for key in
                                          ("command", "group", "outcome", "elapsed_s", "reason")) + " |")
    else:
        lines.append("No finite command results were recorded; a plan alone is not execution evidence.")
        if report.get("plan"):
            lines.extend(["", "Planned commands (not execution evidence):", "",
                          "| Command | Phase | Writes EEPROM | Purpose |", "| --- | --- | --- | --- |"])
            for row in report["plan"]:
                lines.append("| " + " | ".join(_text(row.get(key)) for key in
                                              ("command", "group", "writes", "purpose")) + " |")
    omitted = report.get("omitted", [])
    if omitted:
        lines.extend(["", "Omitted procedures:", ""])
        lines.extend(f"- {_text(row.get('command'))}: {_text(row.get('outcome'))}; {_text(row.get('reason'))}." for row in omitted)
    lines.extend(["", "## Read-only soak", ""])
    if soak:
        lines.extend(["| Measure | Recorded value |", "| --- | --- |"])
        for key, label in (
            ("outcome", "Outcome"), ("requested_s", "Requested seconds"),
            ("elapsed_s", "Elapsed seconds"), ("overrun_s", "Overrun seconds"),
            ("duration_met", "Requested duration met"), ("cycles", "Complete cycles"),
            ("commands", "Commands attempted"), ("read_bytes", "Compared read bytes"),
            ("crc_bytes", "CRC checked bytes"), ("latency_min_s", "Minimum command seconds"),
            ("latency_mean_s", "Mean command seconds"), ("latency_max_s", "Maximum command seconds"),
            ("worst_command", "Slowest command"), ("skipped", "Unsupported telemetry"),
            ("heap", "Heap evidence"), ("console", "Last console evidence"),
            ("reason", "Stop reason"),
        ):
            if key in soak:
                lines.append(f"| {label} | {_text(soak[key])} |")
        if soak.get("failure"):
            lines.extend(["", f"First failed soak command: {_text(soak['failure'])}."])
        if soak.get("per_command"):
            lines.extend(["", "Per-command timing aggregates:", "",
                          "| Command | Recorded measurements |", "| --- | --- |"])
            lines.extend(f"| {_text(command)} | {_text(measurements)} |"
                         for command, measurements in sorted(soak["per_command"].items()))
    else:
        lines.append("No soak result was recorded.")
    if report.get("capture_error") or report.get("cleanup"):
        lines.extend(["", "## Capture and cleanup", ""])
        if report.get("capture_error"):
            lines.append(f"Capture error: {_text(report['capture_error'])}.")
        if report.get("cleanup"):
            lines.append(f"Cleanup evidence: {_text(report['cleanup'])}.")
    lines.extend(["", "## Reproduction", ""])
    if report.get("configuration"):
        lines.extend(["Effective settings:", "", "| Setting | Value |", "| --- | --- |"])
        lines.extend(f"| {_text(key)} | {_text(value)} |" for key, value in sorted(report["configuration"].items()))
        lines.append("")
    if provenance:
        lines.extend([
            _text(provenance.get("scope", "Host provenance does not identify flashed firmware.")), "",
            "| Host evidence | Recorded value |", "| --- | --- |",
        ])
        for key, label in (("argv", "Exact argument vector"), ("python", "Python"), ("os", "OS"),
                           ("pyserial_version", "Installed pyserial"), ("repository", "Repository"),
                           ("sources", "Source SHA-256 hashes")):
            lines.append(f"| {label} | {_text(provenance.get(key))} |")
    else:
        lines.append("Host provenance was not recorded. The firmware label above is operator supplied.")
    lines.extend([
        "", "## Artifacts and evidence limits", "",
        "Files are beside this report. report.json is the machine-readable checkpoint; "
        "it is authoritative if this summary is stale. A completed qualification requires "
        "outcome=PASS and finalized=true, with the requested scope and soak duration satisfied. "
        "transcript.txt contains the finite campaign's serial traffic when captured. "
        "array-before.bin is the complete original byte capture when available. "
        "An authorized programming run also saves scratch-preimage.bin and scratch-preimage.json before writing.", "",
        "A requested soak writes soak-events.jsonl and soak-transcript.txt, each with at most one "
        ".previous segment. Rotation retains recent evidence; the JSON report retains bounded aggregate totals.", "",
        "A passing serial run does not establish power-cycle retention, write-protect wiring, "
        "electrical margins, or behavior on other hardware. Preserve the preimages if any programming "
        "procedure fails; the runner does not automatically restore, retry, or reconnect.", "",
    ])
    return "\n".join(lines)
