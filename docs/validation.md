# Validation results

Local validation in this `24Cxx` checkout on 2026-09-22 (Windows, GCC 15.1
host compiler). These results were rerun here after importing the sibling
implementation; they are not inherited build claims.

| Check | Result |
| --- | --- |
| CMake native core regression suite | Passed, 12 test groups |
| Exhaustive signed temperature decoding | Passed all 4,096 normal and 8,192 extended codes |
| Shared CLI behavioral tests | Passed help/ANSI formatting, color-off, cached diagnostics, invalid arguments, overflow rejection, end/unbind configuration retention and transient one-shot watch recovery |
| Strict C++17 host warnings | Passed `-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror` |
| PlatformIO native tests | Passed all 12 test groups |
| Framework-free compile/link | Passed; no Arduino/Wire headers, including Xtensa macro-collision regression |
| Arduino ESP32-S3 and ESP32-S2 firmware | Compiled and linked with pioarduino 55.03.311 / Arduino 3.3.11 |
| Native ESP-IDF CMake, compilation and firmware link | Passed S3 and S2 with IDF 5.5.5 through the separate PlatformIO `framework = espidf` project; no Arduino facade |
| ESP-IDF component discovery under a different checkout name | Passed actual firmware builds with component name `24Cxx` |
| Release metadata and core boundary checks | Passed |
| PlatformIO release package | Created, checked and independently built with CMake; reference binaries and development tests excluded |
| TI reference integrity | Passed 30 manifest artifacts and 35 SHA-256 entries; all four sensor PDFs re-downloaded from TI match the archive |
| Physical sensor / ALERT / address straps | Not run; no hardware results claimed |

Regression tests cover wire byte order and framing, signed conversion, threshold
rounding and overflow, invalid enum/address/timeouts without I2C, lifecycle,
probe versus tracked health, passive OFFLINE recovery, preserved failed outputs,
partial writes, dirty-state recovery, OS readiness, conversion deadlines, clock
wraparound, extended-format threshold preservation, all AL/POL combinations, and
each initialization transfer failing in turn. A dedicated device model reproduces
TI's premature EM marker; a failure after EM changes must retain the fresh-
conversion requirement through recovery.

CLI regressions exercise rejected mode/shutdown/threshold commands after both
`end` and `unbind`, preserving transport and a staged address. Injected failures
at CONFIG and TEMP reads prove shutdown watch can rejoin a pending conversion.
The new lifecycle regression fails against the original imported CLI and passes
against the corrected implementation.

The full local IDF builds used the repository's actual component registration,
native `app_main`, bootloader and firmware link. Running the standalone `idf.py`
front end separately was not necessary and is not claimed. Other IDF versions
(5.3.2, 5.5.1 and 6.0.1) are configured for S2/S3 in CI but were not run locally
or remotely during setup. Host ASan/UBSan are configured in Linux CI and are
not claimed as locally run Windows sanitizer tests.

Reproduce native checks with the commands in the root README. Firmware builds:

```powershell
.\scripts\pio.cmd run -e esp32s3dev -e esp32s2dev
.\scripts\pio.cmd run --project-dir examples/esp_idf/basic -e esp32s3 -e esp32s2
```

Local build logs are retained in ignored `build/arduino-build.log` and
`build/idf-build.log`. `tools/check_idf_sdk_compile.py` remains an optional,
narrower SDK-header check; it is not a substitute for the full IDF builds above.

Hardware procedure: [hardware-validation.md](hardware-validation.md).
