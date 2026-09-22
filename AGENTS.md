# EEPROM24Cxx repository conventions

- This repository implements 24Cxx I2C EEPROMs, defaulting to the exact Zetta
  ZD24C02B-MAGMT geometry. The former TMP1x2 implementation is Git history only;
  its vendor references are preserved under `docs/archive/`.
- Public headers belong in `include/EEPROM24Cxx/`, implementation in `src/`.
  The C++17 core has no Arduino, ESP-IDF, logging, allocation, tasks, bus handles,
  platform delays, pin initialization or internal bus ownership.
- Follow MB85RC's typed terminal transport and write-effect evidence conventions,
  structured Status, typed enums, passive four-state health and memory vocabulary.
  Application callbacks own serialization, timeouts, pins, clock and recovery;
  callbacks must not retain borrowed buffers, retry secretly or re-enter.
- Bind, operation admission, cancellation, result consumption and teardown are
  bus-silent. Owner-driven poll performs bounded transfers. Compatibility probe,
  begin and recover must clearly document their synchronous transfer behavior.
- EEPROM writes must split at physical page, address-bank and transport limits.
  Do not wrap ranges, guess capacity by writing, or infer identity from an ACK.
  Address-pin meanings, page sizes and upper bank bits vary by manufacturer.
- STOP starts a write cycle. Preserve its wait barrier through failures,
  cancellation and teardown. Never automatically replay an ambiguous write.
  ACK/elapsed write time does not prove contents changed: WP may suppress writes.
  Distinguish accepted bytes, completed work and readback-verified bytes.
- Expected address NACKs during a known ACK-poll window are busy observations,
  not transport-health failures. Generic NACKs and ordinary access failures must
  not be silently reclassified. Probes bypass tracked health.
- No fictitious register map, chip ID, reset, erase or security-lock commands.
  Raw main-array byte addressing is the common protocol; vendor extras need an
  explicitly documented implementation and must never run implicitly.
- Examples share a framework-neutral CLI with the sibling help layout, ANSI
  colors, strict parsers and diagnostics. Startup and ordinary diagnostics never
  program EEPROM. Explicit write/fill commands are the mutation paths.
- Use `library.json` as version source; regenerate with
  `python scripts/generate_version.py sync`, never edit Version.h manually.
- On Windows use `scripts/pio.cmd`, the existing VS Code-managed PlatformIO.
  Do not install another PlatformIO Core.
- Run native protocol/fault/CLI tests, framework-boundary checks and ESP32-S2/S3
  Arduino/native-IDF builds after relevant changes. Record actual results;
  mocks, syntax checks and CI configuration are not physical hardware evidence.
- Vendor reference artifacts retain original licenses. Preserve source URLs,
  revisions and SHA-256 checksums. Reference source is excluded from builds.
