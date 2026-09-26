# Local library comparison

The sibling Projects directory was audited again on 2026-09-26. Sixteen existing
standalone I2C libraries were found; application repositories, dependencies,
archives and worktrees are excluded. `EEPROM_24Cxx` is empty.

The [audit findings and parity matrix](audit-2026-09-26.md) record the inspected
versions, concrete gaps, implemented fixes and intentional protocol differences.

| Library | Protocol | Relevance to 24Cxx EEPROM |
| --- | --- | --- |
| **MB85RC** | I2C FRAM memory | Closest overall: byte addressing, bank mapping, typed transport, partial write evidence, owner-polled jobs and memory CLI |
| OPT4001 | TI light sensor | Shared Status/Config conventions and exact CLI colors/help columns |
| ADS1115 | TI ADC | Bus-silent admission, bounded owner polls and retained results |
| SHT3x-main | Temperature/humidity | One framework-neutral command processor for Arduino and native IDF |
| TMP1x2 | Temperature | Previous checkout scaffold; unrelated sensor protocol replaced |
| BME280 | Temperature/humidity/pressure | Separation of health from configuration/operation state |
| INA228 | Power monitor | Bounded cooperative jobs and passive health |
| INA3221 | Triple power monitor | Typed transport and configuration provenance |
| LDC1614 | Inductance converter | Explicit device variants and owner scheduling |
| LSM6DS3TR | IMU | Fixed-memory operation state and framework boundary |
| MCP45HVX1 | Digital potentiometer | Typed operations and bounded jobs |
| PCA9555 | GPIO expander | Transport result versus physical write effect |
| RV3032-C7 | RTC | Status, health and application-owned timing |
| SCD41 | CO2 sensor | Delayed protocol completion and owner polling |
| SSD1315 | OLED | Transfer budgeting and fixed buffers |
| TCA9548A | I2C switch | Shared-bus ownership discipline |

AT21CS11 is also an EEPROM library, but uses a timed single-wire protocol. Its
write-cycle and irreversible-effect documentation is useful comparison material;
its bit timing, discovery, security and ROM commands do not apply to 24Cxx.
ADS1261_ESP32/MAX31865 use SPI; AsyncSD uses SD; the other standalone libraries
use serial, LED or time services. They are not I2C EEPROM implementations.

## Chosen contracts

MB85RC's `include/MB85RC/{Config,Status,MB85RC}.h`, README and memory CLI supplied
the closest API and ownership vocabulary. OPT4001's `examples/common/CliStyle.h`
and help renderer supplied terminal style. SHT3x's shared CLI supplied the
framework separation pattern. Sibling repositories remain read-only references,
not runtime dependencies.

- Public headers under `include/EEPROM24Cxx/`, implementation under `src/`;
  example adapters and shared CLI under `examples/`.
- `Status { Err code; int32_t detail; const char* msg; }`, static messages,
  `ok()`, `is()`, `inProgress()`, typed enums and camelCase methods/fields.
- Typed terminal transport results retain byte counts and write-effect evidence.
  A callback is one bounded physical transaction; the application owns bus setup,
  pins, speed, serialization, deadlines and recovery. No framework types enter
  public headers.
- Bus-silent operation admission; `poll(nowMs, maxTransactions)` is called by
  the serialized bus owner. One operation per instance, fixed memory and retained
  completion results. No hidden tasks, retries or allocation.
- Passive UNINIT/READY/DEGRADED/OFFLINE health and saturating counters. Expected
  address NACKs during a known EEPROM write-cycle poll are reported as busy;
  they do not establish an offline device. Probe and cached diagnostics are
  distinct from tracked operations.
- Common cached `driverState`, `getConfig`, `getSettings`, timestamp/error and
  counter getters; memory capacity/chunk getters; enum names for diagnostics.
- Request correlation and qualified poll/cancel/timeout/result handling. Both
  original write and readback statuses survive logical cancellation/deadlines.
- Cyan title/commands, green section headings, 32-character command column,
  plain descriptions and `> ` prompt. ANSI red 31, green 32, yellow 33, cyan 36,
  gray 90 and reset 0; `color off` removes ANSI styling. Severity tags, health
  and results use the same meaning on Arduino and native IDF.
- Common help/version/init/bind/end/settings/health/probe/recover aliases and
  memory-specific read/write/fill/verify commands. Both firmware adapters run
  the same C++ command processor, not independently maintained parsers.
- Hex/ASCII dumps, escaped text, printable-string inspection, CRC32, profile
  listing and read-only selftest/stress, with bounded cooperative reads.
- `library.json` is the version source; generated Version.h, native CMake,
  ESP-IDF component metadata, managed PlatformIO wrapper and S2/S3 examples.

## EEPROM differences that must remain explicit

FRAM writes complete at the bus transaction; EEPROM STOP starts a programming
cycle. EEPROM writes must be split at page boundaries or the chip wraps within
its page. This library adds page-aware writes, bounded write-cycle scheduling,
ACK polling and readback verification. It never copies FRAM ID/sleep/high-speed
commands into an EEPROM protocol.

24Cxx is a family convention, not one universal geometry. Page sizes, available
address pins, maximum write times and upper memory-address bit positions differ
by vendor. The exact Zetta preset is the default. Generic presets document their
layout and custom geometry is available; no capacity detection is attempted.
Acknowledged writes may be suppressed by WP, so only successful readback proves
that the requested bytes are present. EEPROM writes consume endurance: ordinary
startup, self-check and stress diagnostics are read-only.

The sibling libraries themselves differ in legacy methods and logical-versus-
physical health accounting. This repository matches the shared design and CLI
conventions while retaining the memory device's actual protocol.
