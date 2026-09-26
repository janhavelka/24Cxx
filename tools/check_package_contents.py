#!/usr/bin/env python3
"""Validate the actual PlatformIO archive, including standalone consumer files.

Archived reference payloads are intentionally repository-only. Their links in
the packaged research index are counted separately, never claimed self-contained.
"""
import json
import posixpath
from pathlib import Path
import re
import sys
import tarfile
from urllib.parse import unquote, urlsplit

REQUIRED = {
    "library.json", "idf_component.yml", "CMakeLists.txt", "LICENSE", "README.md",
    "CHANGELOG.md", "src/EEPROM24Cxx.cpp", "include/EEPROM24Cxx/EEPROM24Cxx.h",
    "include/EEPROM24Cxx/Config.h", "include/EEPROM24Cxx/Types.h",
    "include/EEPROM24Cxx/Status.h", "include/EEPROM24Cxx/Version.h",
    "include/EEPROM24Cxx/CommandTable.h", "examples/README.md",
    "examples/01_basic_bringup_cli/main.cpp", "examples/common/BoardConfig.h",
    "examples/common/Eeprom24CxxCli.h", "examples/common/Eeprom24CxxCli.cpp",
    "examples/common/WireTransportHelpers.h", "examples/common/IdfTransportHelpers.h",
    "examples/esp_idf/basic/CMakeLists.txt", "examples/esp_idf/basic/main/CMakeLists.txt",
    "examples/esp_idf/basic/main/main.cpp", "examples/esp_idf/basic/sdkconfig.defaults",
    "docs/integration.md", "docs/validation.md", "docs/reference/README.md",
}
FORBIDDEN_PARTS = {".git", ".github", ".pio", ".vscode", "managed_components", "__pycache__"}
FORBIDDEN_ROOTS = {"test", "tools", "scripts"}
FORBIDDEN_FILES = {"AGENTS.md", ".gitignore", ".gitattributes", "Doxyfile", "platformio.ini"}
GENERATED_FILES = {"sdkconfig", "sdkconfig.old", "dependencies.lock", "compile_commands.json"}


def main():
    if len(sys.argv) != 2:
        print("Usage: python tools/check_package_contents.py <archive.tar.gz>", file=sys.stderr)
        return 2
    archive = Path(sys.argv[1])
    errors = []
    reference_links = 0
    with tarfile.open(archive, "r:gz") as package:
        entries = {item.name.removeprefix("./"): item for item in package.getmembers()}
        files = {name for name, item in entries.items() if item.isfile()}
        for name in sorted(REQUIRED - files):
            errors.append(f"missing consumer file: {name}")
        for name, item in entries.items():
            parts = name.split("/")
            if name.startswith("/") or ".." in parts or item.issym() or item.islnk():
                errors.append(f"unsafe archive entry: {name}")
            if (set(parts) & FORBIDDEN_PARTS or parts[0] in FORBIDDEN_ROOTS or
                    name in FORBIDDEN_FILES or parts[-1] in GENERATED_FILES or
                    any(part.startswith("build") for part in parts[:-1]) or
                    name.startswith(("docs/reference/source/", "docs/reference/datasheets/", "docs/archive/"))):
                errors.append(f"repository/generated content leaked into package: {name}")
        if "library.json" in files:
            manifest = json.load(package.extractfile(entries["library.json"]))
            if manifest.get("name") != "EEPROM24Cxx" or not re.fullmatch(r"\d+\.\d+\.\d+", manifest.get("version", "")):
                errors.append("invalid package identity/version")
        for name in sorted(files):
            if not name.endswith(".md"):
                continue
            text = package.extractfile(entries[name]).read().decode("utf-8")
            for match in re.finditer(r"\[[^\]]+\]\(([^)]+)\)", text):
                target = match.group(1).strip().strip("<>").split(' "', 1)[0]
                parsed = urlsplit(target)
                if parsed.scheme or parsed.netloc or not parsed.path:
                    continue
                resolved = posixpath.normpath(posixpath.join(posixpath.dirname(name), unquote(parsed.path)))
                if resolved in files or any(entry.startswith(resolved.rstrip("/") + "/") for entry in files):
                    continue
                if resolved.startswith(("docs/reference/", "docs/archive/")):
                    reference_links += 1
                    continue
                errors.append(f"broken consumer documentation link in {name}: {target}")
    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1
    print(f"Package contracts passed: {len(files)} files; {reference_links} repository-only reference links")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
