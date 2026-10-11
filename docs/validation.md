# Validation results

## CLI procedure audit: 2026-10-11

The [sixteen-library CLI comparison](library-comparison.md#cli-and-procedure-audit-2026-10-11)
led to deeper scratch procedures, correlated whole-command results, manual
owner controls, conservative runtime protocol settings and a serial runner.
The framework-neutral core API and implementation are unchanged by this pass.

Local checks after these changes:

- Strict GCC 15.1.0 C++17 CMake/Ninja build with conversion warnings treated as
  errors: all seven CTest entries pass (core, CLI, transport, helpers, scratch
  diagnostics, Python runner and real CLI transcripts).
- Managed PlatformIO native: all five suites discovered, all 28 cases pass.
  The separate framework-free core compile/link target passes.
- Runner: 36 offline tests cover complete byte evidence, CRC, all scratch
  checks/counts, partial replies/writes, early prompts, missing/mismatched/reused
  completion IDs, resets, foreign active work, strict arguments and durable
  backup ordering. No failure cleanup sends bytes to a possibly partial command
  or cancels another owner's job.
- The actual shared C++ CLI, connected to a host EEPROM fixture, passes 18
  targeted transcript checks plus a complete runner campaign. This exercises
  the runner's real plan and preimage files, and independently compares the full
  modeled array before/after all scratch procedures.
- New scratch fault injection corrupts a neighboring byte while the addressed
  write verifies successfully. Independent shadow comparison detects the
  corruption before restoration, retains the backup, and explicit restoration
  succeeds without erasing the original failure. Every owner tick is checked
  for at most one physical callback.
- Runtime-control regressions cover adapter capacity limits, unavailable probe
  callbacks/counters, timed fallback, paused deadline expiry, uint32_t clock
  rollover and physical callback success after the CLI deadline has elapsed.
- ESP32-S2 and S3: full Arduino 3.3.11 and native ESP-IDF 5.5.5 compile/link and
  binary generation pass. These are firmware builds, not physical trials.
- Release/framework contracts and all reference checksums pass. Package export
  validates 42 files, and the extracted standalone library builds with strict
  warnings. Read-only and write-enabled runner dry runs produce `NOT_RUN`
  artifacts without opening a serial port.

Build logs, package checks and dry-run artifacts are ignored under
`build-cli-audit/`. The runner is repository tooling, not part of the embedded
package. Python 3.10+ is required for host tests; only live serial campaigns
require `pyserial`. The [bench procedure](hardware-validation.md) distinguishes
successful serial evidence from electrical, WP, retention and power-cut tests.
No firmware was flashed and no real EEPROM or FieldCore hardware was exercised.

## Maintenance audit: 2026-10-11

The audit simplified private driver state, shared staged read transactions and
write-effect validation, blocking completion, CLI aliases and adapter diagnostics.
The public core API and snapshot fields remain unchanged. On the host x64 GCC
build, `sizeof(EEPROM24Cxx)` decreased from 816 to 704 bytes; this is a host ABI
measurement, not an ESP32 RAM measurement.

A new regression reproduced `BlockingMemory::waitUntilReady()` returning success
after an application wait overshot the caller's deadline. The corrected helper
returns TIMEOUT while allowing an elapsed physical barrier to retire, preserving
the prior transfer result and issuing no I2C. Tests also cover active/retained
foreign requests and stalled clocks. The CLI scan fixture now
preserves timeout classification and checks that scans do not alter driver health.

Checks run after the cleanup:

- Strict GCC 15.1.0 C++17 CMake/Ninja build: all four CTest suites passed.
- Managed PlatformIO native tests: all 27 cases passed; the framework-free
  compile/link target also passed.
- ESP32-S2 and S3: full Arduino 3.3.11 and native ESP-IDF 5.5.5 builds passed.
- Release/framework contracts, generated-version checks and reference checksums
  passed. The version script also passed standalone import/bump smoke checks.
- Exported package: 42 files validated, including the shared transport diagnostics
  header; the extracted CMake library compiled with strict warnings.
- Independent review checked read staging, callback budgets, timing, passive
  health, write-effect normalization and retained physical barriers.

Build artifacts and PlatformIO logs are ignored under `build-maintenance/`.
The old syntax-only IDF checker was removed because full native builds now cover
that path. GitHub Actions supplies the Linux sanitizer and IDF version matrix
for each pushed revision. No board was flashed; no electrical, WP, power-cut or
FieldCore firmware integration result is claimed. The FieldCore adapter contract
is documented in [integration](integration.md#fieldcore-node-integration).

## Previous validation: 2026-09-26

Validated on 2026-09-26 after the [sibling-library audit](audit-2026-09-26.md)
and fixes. No board was flashed and no physical EEPROM/WP result is claimed.

## Host and package checks

- GCC 15.1.0, C++17, CMake/Ninja: core and shared CLI compile with
  `-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror`.
- CTest: all four executables pass (core, shared CLI, transport and field helpers).
  The core has 24 regression groups; the CLI suite exercises command processing
  against an EEPROM model, transport tests exercise the firmware helpers, and
  field helpers exercise typed storage, CRC, update and bounded blocking waits.
- Managed PlatformIO `test -e native`: all four suites are discovered and pass,
  reporting 27 test cases (24 core groups, CLI, transport and helpers). Previously the
  nonstandard CLI directory was silently skipped; all suites now use test_*
  directories.
- Managed PlatformIO `run -e native_core_no_arduino`: strict compile/link passes
  with no Arduino, Wire or ESP-IDF include paths or dependencies.
- `python tools/check_contracts.py`: release metadata and framework boundaries pass.
- `python tools/check_reference_archive.py`: 15 document artifacts (14 PDFs and
  the catalogue), 28 source/license artifacts and 66 archive checksums pass.
- PlatformIO package export: 41 files, expected public/core/example files
  present, vendor source/PDFs and generated build artifacts excluded.
  `tools/check_package_contents.py` validates the actual archive and documents
  eight intentionally repository-only reference links. The freshly extracted
  package builds as a standalone CMake C++17 library with strict warnings;
  a separate consumer links and executes the public blocking helper API.

The subsequent GitHub native run on commit `864cd84` exposed test-model integer
portability errors under Linux GCC 13.3.0: shifting uint8_t values promoted them
to signed int, while unsigned-long masks/minimums depended on Windows' narrower
long type. Bank calculations now use explicit uint32_t operands throughout.
After the fix, all four strict local CTest suites and all 27 PlatformIO native
cases pass again, as do contract/reference checks. The two changed fixtures also
pass local syntax checks with the workflow's warning and sanitizer flags.
The [subsequent GitHub run](https://github.com/janhavelka/24Cxx/actions/runs/36258748517)
passed all jobs, including Linux ASan/UBSan tests. The second datasheet/structure
review changes are validated locally as listed here; that older CI run does not
validate the new unpushed GPIO, CLI or source-layout changes.

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
Current-address tests cover pure-read opt-in, exact Zetta page/array pointer
wrap, conservative generic invalidation, transport faults, barriers and callback
budgets. Update tests cover all-equal skips, changed chunks, comparison failure,
WP mismatch, cancellation, retained evidence and deadlines.

CLI regressions cover startup without programming, ANSI colors/help columns,
read-only diagnostics/stress, strict decimal/hex parsing (including leading
zeros), malformed/overlong/control-character input, borrowed-buffer protection,
page-split programming, write protection, partial-write effect reporting,
bank selection and cancellation. New checks cover full-range hex/ASCII views,
escaped text, strings crossing buffers/banks, CRC32 reference vectors, partial
CRC suppression, stable range IDs, staged/active settings and direct-input
control characters. The full Arduino builds caught a HEX macro collision in
the view enum; it was fixed and a header regression was added. Expanded tests
cover tracked startup and failed presence, full-array selftest across banks,
current/update/watch, counters/assertions, WP/reset hooks and post-reset settling.
Scratch suites test distinct patterns and typed layouts, backup and restoration
failures, WP suppression, cancellation before backup/during programming/during
restoration, end/rebind retention, mutation gates, and scan blocking during a
cancelled write's remaining programming cycle.
The second review added setup/reconfiguration regressions for active operations,
retained backups, physical/reset barriers, invalid replacement and stale input,
plus preservation of independent primary/restore terminal evidence after reads.

Field-helper tests cover explicit endian/IEEE encodings, signed extrema, CRC
reference and split-update vectors, failed-output preservation, page-crossing
typed writes, range/single-chunk validation, current reads, skipped updates,
WP and ambiguous-write evidence, stalled clocks, logical deadlines including a
compound CRC deadline, foreign active-request preservation and explicit idle
barrier settling without replay.

The dedicated transport suite checks the same Wire/IDF helper implementations
used by firmware: readiness/argument validation, timeout restoration, deferred
repeated START, short/absent buffers, unsent-data cleanup, missing received bytes,
ambiguous NACK mapping and write-effect evidence. Pure reads check zero pointer
traffic and exact received counts. GPIO recovery tests cover bounded clock
pulses, the required START then STOP, SDA release on the ninth pulse/stuck low,
SCL timeout and timer rollover. WP tests check protected preload before output
enable, explicit setup/hold settling, level mismatch, failed GPIO setup and
unavailable control after failed initialization. These tests do not emulate
electrical bus timing or prove SDK/controller behavior on hardware.

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
not run locally. The successful GitHub run linked above passed native IDF 5.3.2,
5.5.1 and 6.0.1 on both targets, both Arduino builds, native sanitizer tests and
package validation. After the second review's edits, local S2/S3 Arduino 3.3.11
and native IDF 5.5.5 builds pass again. The new revision's complete Linux/SDK
matrix still requires CI after pushing; sanitizers were not run on Windows.

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
