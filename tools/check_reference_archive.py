#!/usr/bin/env python3
"""Verify the current EEPROM reference archive without network access."""
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1] / "docs" / "reference"


def check_file(relative, digest, size=None):
    path = (ROOT / relative).resolve()
    if not path.is_relative_to(ROOT.resolve()):
        raise ValueError(f"Reference path escapes archive: {relative}")
    data = path.read_bytes()
    if hashlib.sha256(data).hexdigest() != digest:
        raise ValueError(f"SHA-256 mismatch: {relative}")
    if size is not None and len(data) != size:
        raise ValueError(f"Size mismatch: {relative}")


def main():
    manifest = json.loads((ROOT / "manifest.json").read_text(encoding="utf-8"))
    documents = 0
    for artifact in manifest["artifacts"]:
        if artifact.get("status") != "archived":
            continue
        check_file(artifact["path"], artifact["sha256"], artifact["bytes"])
        if "text_path" in artifact:
            check_file(artifact["text_path"], artifact["text_sha256"])
        documents += 1
    repositories = json.loads((ROOT / "repository-inventory.json").read_text(encoding="utf-8"))
    sources = 0
    for repository in repositories["repositories"]:
        for artifact in repository.get("artifacts", []):
            if "sha256" in artifact:
                check_file(artifact["path"], artifact["sha256"], artifact["bytes"])
                sources += 1
    checksums = 0
    covered = set()
    for line in (ROOT / "SHA256SUMS").read_text(encoding="utf-8").splitlines():
        if not line.strip():
            continue
        digest, relative = line.split(maxsplit=1)
        relative = relative.lstrip("*")
        check_file(relative, digest)
        covered.add(relative)
        checksums += 1
    actual = {path.relative_to(ROOT).as_posix() for path in ROOT.rglob("*")
              if path.is_file() and path.name != "SHA256SUMS" and "__pycache__" not in path.parts}
    if missing := actual - covered:
        raise ValueError(f"Unhashed reference files: {sorted(missing)}")
    print(f"EEPROM reference integrity passed: {documents} documents, {sources} source artifacts, {checksums} checksums")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
