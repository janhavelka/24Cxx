# Changelog

## Unreleased

### Fixed

- Preserve the physical write-cycle barrier for unknown transport outcomes;
  malformed results can no longer establish that a write had no effect.
- Preserve distinct write/readback statuses, report mismatch offsets, and reset
  stale geometry at teardown without discarding pending write/result evidence.
- Harden Wire buffer/timeout handling and preserve typed native IDF NACK errors.
- Reject unsupported characters through both CLI input entry points.
- Fix PlatformIO native discovery so CLI and transport tests actually run.

### Added

- Sibling health/config/settings/memory getters and enum diagnostic names.
- Correlated requests, qualified polling/cancellation/result consumption, and
  owner-declared timeouts for clockless integrations.
- Full-range cooperative hex/ASCII, text, strings and CRC32 CLI tools; variants,
  size, heap, verbose and read-only selftest diagnostics.
- Transport regression suite, reproducible exported-package checks, and the
  documented sixteen-library audit and validation results.

## EEPROM24Cxx 1.0.0 - 2026-09-22

### Added

- Zetta ZD24C02B default and explicit 24Cxx geometry profiles.
- Framework-neutral, owner-polled EEPROM reads, page writes, fill and verification.
- Typed transport outcomes, write-effect evidence, write-cycle handling and passive health.
- Shared colored Arduino/native ESP-IDF diagnostic CLI, native tests and S2/S3 builds.
- Manufacturer datasheets and source research with provenance and checksums.

### Changed

- Corrected the library target from the mistakenly requested TMP102/TMP112
  temperature sensors to 24Cxx EEPROM memory. The former API is removed;
  its original commit remains in Git history.
