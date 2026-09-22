# Changelog

## 1.0.0 - 2026-09-22

### Added

- TMP102 and TMP112 typed register driver with normal/extended temperature decoding,
  conversion rates, shutdown/one-shot operation, thresholds and ALERT configuration.
- Framework-neutral transport injection, passive health tracking, explicit recovery
  and configuration verification.
- Arduino and native ESP-IDF diagnostic examples with shared CLI conventions.
- Native protocol/failure tests and archived TI reference material.
- Native ESP-IDF PlatformIO project and support for arbitrary checkout folder names.

### Fixed

- CLI configuration retained after rejected operations on an unbound or ended driver.
- Shutdown-mode watch can resume a pending conversion after a transient read failure.
