# Changelog

## Unreleased

### Fixed

- Reject duplicate HIL evidence fields and inconsistent write-commit, byte-count
  or stage records; recheck saved preimages before programming and soaking.
- Checkpoint each finite HIL step before sending commands, and prevent report
  summary failures from leaving a newly published JSON PASS.
- Replace blocking/competing example consoles with an explicit console owner,
  bounded partial output and input polling; prevent detected RX loss from
  executing a damaged command. Configure S2/S3 control lines and native IDF
  console/power defaults explicitly for sustained runs.
- Freeze completed CLI elapsed time and detect inconsistent repeated stress
  reads without conflating content failures with successful physical transfers.
- Keep HIL framing and session correlation bounded over long runs; preserve
  proven failures when subsequent artifact or serial-close operations fail.
- Reject a blocking readiness wait that finishes after its caller's deadline,
  even when the physical programming barrier expired during the wait.
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

### Changed

- Store only passive health telemetry in each driver, keeping the public settings
  snapshot unchanged; share staged read handling and isolate write-effect validation.
- Consolidate blocking result consumption, CLI command aliases, adapter counters
  and probe diagnostics; reuse Wire initialization for startup and recovery.
- Remove duplicate CLI transport checks, migration-era version-generation
  fallbacks and the obsolete IDF syntax-only checker. Full firmware builds remain
  the platform validation path; archived vendor provenance stays preserved.
- Document FieldCore-node ownership, routed callbacks, wrapping clocks and
  retained programming barriers without adding firmware policy to the library.
- Move non-template blocking helper execution into `src/BlockingMemory.cpp`;
  retain public declarations and typed codec templates in the header.
- Share tested WP and electrical recovery policies between ESP32 frameworks,
  and include the new source/helpers in all build and package checks.
- Add repository/homepage metadata for package consumers.

### Added

- Python HIL firmware expectations, read-only rejection matrix, optional heap
  limits and physical-write counter checks; grouped host help and progress.
- JSON/Markdown qualification reports with host provenance, explicit omitted
  procedures, cleanup finalization and per-command soak timing; `--require-run`
  prevents a dry run from returning a successful automation exit code.
- Topic/per-command help, read-only quickstart, `detect`, targeted address probes,
  bank coverage and per-address error summaries, legal bases and pointer mapping.
- Rotating paired read stress with finite intervals, progress/latency/byte metrics,
  and console queue/loss/capability telemetry.
- Timed read-only Python soak campaigns with independent baseline/CRC checks,
  heap/health/console sampling, bounded logs and atomic report checkpoints.
- Detection/console regressions, actual-CLI soak transcripts and documented USB
  backpressure/disconnect/long-run bench procedures.
- Whole-command CLI session IDs and terminal outcomes, serial framing markers,
  manual polling/timeout controls, cached bus configuration and an overall deadline.
- Ended-only transfer limits, ACK-poll/timed-wait selection and conservative
  write-cycle overrides for adapter and scheduler testing.
- Expanded scratch procedures: cooperative ownership checks, nine-operation
  mixed cycles, random shadow-window verification and all public typed codecs.
- A bounded serial HIL runner with explicit write opt-in, durable preimages,
  strict stage/restoration evidence and offline/actual-CLI transcript tests.
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
