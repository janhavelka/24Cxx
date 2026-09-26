# Validation results

Validated on 2026-09-26 after the [sibling-library audit](audit-2026-09-26.md)
and fixes. No board was flashed and no physical EEPROM/WP result is claimed.

## Host and package checks

- GCC 15.1.0, C++17, CMake/Ninja: core and shared CLI compile with
  `-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror`.
- CTest: all three executables pass (core, shared CLI and transport).
  The core has 17 regression groups; the CLI suite exercises command processing
  against an EEPROM model, and transport tests exercise the firmware helpers.
- Managed PlatformIO `test -e native`: all three suites are discovered and pass,
  reporting 19 test cases (17 core groups, CLI and transport). Previously the
  nonstandard CLI directory was silently skipped; all suites now use test_*
  directories.
- Managed PlatformIO `run -e native_core_no_arduino`: strict compile/link passes
  with no Arduino, Wire or ESP-IDF include paths or dependencies.
- `python tools/check_contracts.py`: release metadata and framework boundaries pass.
- `python tools/check_reference_archive.py`: 15 document artifacts (14 PDFs and
  the catalogue), 28 source/license artifacts and 66 archive checksums pass.
- PlatformIO package export: 33 files, expected public/core/example files
  present, vendor source/PDFs and generated build artifacts excluded.
  `tools/check_package_contents.py` validates the actual archive and documents
  seven intentionally repository-only reference links. The freshly extracted
  package builds as a standalone CMake C++17 library with strict warnings.

The core model emulates page wrapping, bank-local reads, write-cycle NACKs and
write protection. Regressions cover every preset, page and bank boundaries,
invalid geometry/ranges, fixed-buffer limits, exact completion counts, partial
reads/writes, ambiguous effects, no replay, readback mismatch, retained results,
cancellation/end barriers, ACK faults, callback budgets, clockless operation,
post-callback deadlines and uint32_t rollover. Health tests distinguish expected
busy polling and logical/content failures from physical transport failures.
Added coverage includes unknown transport codes falsely claiming no write,
retained write/readback statuses, bank-specific ACK polling, repeated teardown,
bus-silent diagnostic getters, explicit/automatic request IDs, stale owner
actions and owner-declared timeouts that preserve programming barriers.

CLI regressions cover startup without programming, ANSI colors/help columns,
read-only diagnostics/stress, strict decimal/hex parsing (including leading
zeros), malformed/overlong/control-character input, borrowed-buffer protection,
page-split programming, write protection, partial-write effect reporting,
bank selection and cancellation. New checks cover full-range hex/ASCII views,
escaped text, strings crossing buffers/banks, CRC32 reference vectors, partial
CRC suppression, stable range IDs, staged/active settings and direct-input
control characters. The full Arduino builds caught a HEX macro collision in
the new view enum; it was fixed and a header regression was added.

The dedicated transport suite checks the same Wire/IDF helper implementations
used by firmware: readiness/argument validation, timeout restoration, deferred
repeated START, short/absent buffers, unsent-data cleanup, missing received bytes,
ambiguous NACK mapping and write-effect evidence. It does not emulate electrical
bus timing or prove SDK/controller behavior on hardware.

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

The Arduino platform emits a host Windows long-path-support warning, and the
tooling prints a console-codepage metrics notice; all four builds complete.
Build logs and the package smoke workspace are local ignored files under
`build-audit/`. Physical electrical/timing/WP validation remains the
[bench procedure](hardware-validation.md), not a result established by mocks.

Additional reproducible checks:

```powershell
cmake -S . -B build-audit -G Ninja -DEEPROM24CXX_BUILD_TESTS=ON '-DCMAKE_CXX_FLAGS=-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror'
cmake --build build-audit --parallel
ctest --test-dir build-audit --output-on-failure
.\scripts\pio.cmd test -e native
.\scripts\pio.cmd run -e native_core_no_arduino
python tools/check_contracts.py
python tools/check_reference_archive.py
.\scripts\pio.cmd pkg pack --output build-audit/EEPROM24Cxx.tar.gz
python tools/check_package_contents.py build-audit/EEPROM24Cxx.tar.gz
```
