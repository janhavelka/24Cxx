# Changelog

## Unreleased

### Fixed

- Complete the Zetta protocol-recovery sequence with START before the final
  idle STOP; use the same electrical recovery in Arduino and native ESP-IDF.
- Enforce WP setup/hold settling and keep failed WP initialization unavailable.
- Preserve CLI target/context during active work, settling and retained scratch
  backups; retain detailed primary/restore evidence across later diagnostics.
- Use explicit uint32_t bank arithmetic in native EEPROM test models, fixing
  Linux GCC conversion errors without weakening CI warnings or sanitizers.
- Preserve the physical write-cycle barrier for unknown transport outcomes;
  malformed results can no longer establish that a write had no effect.
- Preserve distinct write/readback statuses, report mismatch offsets, and reset
  stale geometry at teardown without discarding pending write/result evidence.
- Harden Wire buffer/timeout handling and preserve typed native IDF NACK errors.
- Reject unsupported characters through both CLI input entry points.
- Fix PlatformIO native discovery so CLI and transport tests actually run.

### Added

- Current-address reads with explicit transport opt-in and conservative pointer
  tracking; direct synchronous reads and init/unbind lifecycle aliases.
- Cooperative compare-before-write updates, skipped-byte/comparison evidence,
  and an optional blocking facade with typed endian-safe storage and CRC32.
- Comprehensive scratch CLI suites, typed demo, finite watch, full-array selftest,
  retained backups and explicit recovery after failed/cancelled programming.
- Tracked startup initialization/health/help, page/timing/threshold diagnostics,
  transfer counters/assertions, optional WP GPIO and interface recovery hooks.
- Pure-read transport and bus-recovery regressions plus a field-helper suite.
- Sibling health/config/settings/memory getters and enum diagnostic names.
- Correlated requests, qualified polling/cancellation/result consumption, and
  owner-declared timeouts for clockless integrations.
- Full-range cooperative hex/ASCII, text, strings and CRC32 CLI tools; variants,
  size, heap, verbose and read-only selftest diagnostics.
- Transport regression suite, reproducible exported-package checks, and the
  documented sixteen-library audit and validation results.

### Changed

- Move non-template blocking helper execution into `src/BlockingMemory.cpp`;
  retain public declarations and typed codec templates in the header.
- Share tested WP and electrical recovery policies between ESP32 frameworks,
  and include the new source/helpers in all build and package checks.
- Add repository/homepage metadata for package consumers.

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
