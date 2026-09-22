# Validation results

Validated on 2026-09-22 after replacing the mistakenly requested sensor driver
with EEPROM24Cxx. No board was flashed and no physical EEPROM/WP result is claimed.

## Host and package checks

- GCC 15.1.0, C++17, CMake/Ninja: core and shared CLI compile with
  `-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror`.
- CTest: both core and CLI executables pass. The core has 11 regression groups;
  the CLI suite exercises command processing against an EEPROM model.
- Managed PlatformIO `test -e native`: all 11 core groups pass.
- Managed PlatformIO `run -e native_core_no_arduino`: strict compile/link passes
  with no Arduino, Wire or ESP-IDF include paths or dependencies.
- `python tools/check_contracts.py`: release metadata and framework boundaries pass.
- `python tools/check_reference_archive.py`: 15 document artifacts (14 PDFs and
  the catalogue), 28 source/license artifacts and 66 archive checksums pass.
- PlatformIO package export: 31 entries, expected public/core/example files
  present, vendor source/PDFs and generated build artifacts excluded. Extracted
  package builds as a standalone CMake C++17 library with strict warnings.

The core model emulates page wrapping, bank-local reads, write-cycle NACKs and
write protection. Regressions cover every preset, page and bank boundaries,
invalid geometry/ranges, fixed-buffer limits, exact completion counts, partial
reads/writes, ambiguous effects, no replay, readback mismatch, retained results,
cancellation/end barriers, ACK faults, callback budgets, clockless operation,
post-callback deadlines and uint32_t rollover. Health tests distinguish expected
busy polling and logical/content failures from physical transport failures.

CLI regressions cover startup without programming, ANSI colors/help columns,
read-only diagnostics/stress, strict decimal/hex parsing (including leading
zeros), malformed/overlong/control-character input, borrowed-buffer protection,
page-split programming, write protection, partial-write effect reporting,
bank selection and cancellation. The Wire adapter helper tests ambiguous error
mapping and clearing an unsent partial buffer before releasing the mutex.

## Firmware builds

The repository wrapper uses the existing VS Code-managed PlatformIO Core. The
platform is pinned to pioarduino Espressif32 55.03.311. Both examples use a 4 MB
flash layout; generated SDK configurations confirm that choice for S2 and S3.

| Framework | Local version | ESP32-S3 | ESP32-S2 |
| --- | --- | --- | --- |
| Arduino-ESP32 | 3.3.11 | Compile/link and binary generation passed | Compile/link and binary generation passed |
| Native ESP-IDF | 5.5.5 | Compile/link, bootloader and binary generation passed | Compile/link, bootloader and binary generation passed |

Commands:

```powershell
.\scripts\pio.cmd run -e esp32s3dev -e esp32s2dev
.\scripts\pio.cmd run --project-dir examples/esp_idf/basic -e esp32s3 -e esp32s2
```

These are full native ESP-IDF component/application builds, not an Arduino
compatibility build or syntax-only check. The separate `idf.py` front end was
not run locally. CI is configured for native IDF 5.3.2, 5.5.1 and 6.0.1 on both
targets; those CI jobs have not been executed in this session. Host sanitizer
checks are also configured in CI but were not run on this Windows host.

The Arduino platform emits a host Windows long-path-support warning; all builds
complete. Build logs and the package smoke workspace are local ignored files
under `build-eeprom/`. Physical electrical/timing/WP validation remains the
[bench procedure](hardware-validation.md), not a result established by mocks.
