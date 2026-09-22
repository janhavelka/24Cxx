# Changelog

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
