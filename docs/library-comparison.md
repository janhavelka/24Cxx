# Local library comparison

Independently inspected on 2026-09-22 in the sibling `Projects/` directory.
The survey enumerated immediate project directories, checked standalone library
manifests and public transport headers, and compared the closest drivers and
their example CLIs. It found 16 existing standalone I2C libraries, including an
exact TMP1x2 implementation. Application dependencies, build outputs, archive
copies and worktrees are excluded from that count. Versions below are the local
`library.json` versions at inspection, not claims about upstream releases.

| Repository | Version | Device / protocol | Relevance to this library |
| --- | --- | --- | --- |
| TMP1x2 | 1.0.0 | TI TMP102/TMP112; four 16-bit pointer registers | Exact device match; source reused for this repository |
| OPT4001 | 1.2.2 | TI light sensor; 16-bit registers, thresholds, one-shot/continuous, ALERT | Closest other-device API, layout, health and CLI template |
| ADS1115 | 2.0.2 | TI ADC; four 16-bit registers at 0x00-0x03, addresses 0x48-0x4B | Closest register-layout/address analogue; more extensive cooperative lifecycle |
| SHT3x-main | 1.9.0 | Temperature/humidity; command words and CRC | Closest measurement domain; shared framework-neutral CLI |
| BME280 | 2.2.0 | Temperature/humidity/pressure; byte registers and compensation | Health/configuration-trust separation, IDF integration |
| INA228 | 3.0.4 | TI power monitor; mixed-width registers | Cooperative jobs, passive health, explicit transfer limits |
| INA3221 | 3.2.0 | TI triple power monitor; 16-bit registers | Register transport, sample/configuration provenance |
| LDC1614 | 3.2.0 | TI inductance converter; 16-bit registers | Typed variants and externally driven cooperative operations |
| LSM6DS3TR | 2.1.0 | IMU; byte registers and FIFO | Framework-neutral callbacks and operation/configuration state |
| MB85RC | 4.2.0 | I2C FRAM family | External ownership and partial-write reporting |
| MCP45HVX1 | 2.0.0 | Digital potentiometer | Typed register API and bounded polling jobs |
| PCA9555 | 3.0.3 | GPIO expander | External ownership and separate transport/write-effect state |
| RV3032-C7 | 3.1.0 | RTC with temperature measurement | Common Status/Config layout and health |
| SCD41 | 1.3.2 | CO2/temperature/humidity; delayed command/CRC frames | Owner-driven lifecycle with a specialized transfer abstraction |
| SSD1315 | 4.0.3 | OLED command/data streams | Cooperative transfer budgeting; different transport result type |
| TCA9548A | 1.1.0 | I2C switch control byte | Bus ownership discipline; different chip protocol |

`EEPROM_24Cxx/` was empty at inspection. This repository, `24Cxx/`, was also
empty before initialization and is not an additional pre-existing library.
Other standalone libraries use SPI (`ADS1261_ESP32`, `MAX31865`), single wire
(`AT21CS11`), E2 signaling (`EE871-E2`), SD (`AsyncSD`), serial protocols
(`SHZK-PT`, `VibWire-108`, `VTN4xx`, `SIM7080G-Core`), LED/RMT (`StatusLED`),
or platform time (`SystemChrono`). `LGClimateLink` is a separate climate-control
protocol project. `ESP32_Interfaces` and `NextionNX3224T028_UART` are applications.
Remaining entries are applications, board/design projects or TunnelMonitor
archives/worktrees.

## Sources for the chosen conventions

The existing TMP1x2 source provides the device implementation and retained
reference archive; its precise revision is recorded in [provenance.md](provenance.md).
Among other devices, OPT4001 is the strongest overall template. ADS1115 supplies
the closest register/address comparison. SHT3x demonstrates a shared command
processor that compiles in Arduino and native IDF without an Arduino facade.
Sensor protocol facts come from [TI references](reference/README.md).

The local comparison used these concrete files:

- `OPT4001/AGENTS.md`, `include/OPT4001/Config.h`, `Status.h`, and
  `examples/common/CliStyle.h` and `Log.h`: callback signatures, structured
  errors, passive transport health, help column width, ANSI palette and prompt.
- `ADS1115/AGENTS.md`, `include/ADS1115/ADS1115.h` and
  `examples/01_basic_bringup_cli/main.cpp`: external bus ownership, four-register
  layout, cooperative production lifecycle and bounded diagnostic CLI.
- `SHT3x-main/AGENTS.md`, `include/SHT3x/SHT3x.h` and
  `examples/common/Sht3xCli.h/.cpp`: framework-neutral CLI, operation polling
  and the distinction between logical/protocol and physical transport health.
- `TMP1x2/include/TMP1x2/`, `src/TMP1x2.cpp`,
  `examples/common/Tmp1x2Cli.h/.cpp`, `platformio.ini` and the native-IDF
  example: the reused implementation and its actual lifecycle/build boundary.

Paths above are relative to the inspected sibling repository, not dependencies
required to build this repository. All listed I2C libraries also expose transport
configuration in their own `include/<namespace>/Config.h`; namespace exceptions
are `RV3032` and lowercase `ssd1315`.

## API, ownership and health compatibility

Public headers live under `include/TMP1x2/`: `TMP1x2.h`, `Config.h`, `Status.h`,
`CommandTable.h` and generated `Version.h`; implementation lives under `src/`.
The C++17 core has no framework headers, logging, platform delay, allocation,
bus handles or pin initialization. Example adapters own platform resources.

The common API vocabulary is `Status { Err code; int32_t detail; const char* msg; }`
with static messages, `ok()`, `is()`, `inProgress()`, explicit bool, `Ok()` and
`Error()` factories. Enums are typed `enum class ... : uint8_t`; public enum
values and register constants are uppercase, methods/fields use camelCase and
members use `_camelCase`.

`I2cWriteFn` and `I2cWriteReadFn` accept a seven-bit address, byte buffers,
lengths, a timeout and an opaque user context. A register read is one atomic
pointer-write/repeated-START/read transaction. The application owns lifecycle,
pins, clock rate, serialization, timeout enforcement, scheduling and recovery.
Callbacks return a terminal result and never re-enter the driver. Platform
errors map to `Status`; a generic NACK must not invent address/data phase evidence.

TMP1x2 follows OPT4001's **bounded synchronous driver** model. `bind()`, `end()`
and `unbind()` are bus-silent. Initialization, recovery and setters perform
synchronous transfers; format/shutdown transitions can also wait using the
injected clock and cooperative-yield hook. `startOneShot()` reads and writes
CONFIG immediately. `tick()` can poll readiness with one transfer, while
`tryRead()` can read CONFIG and TEMP in one call. Conversion waiting is split
across application calls, but this API does not supply an ADS1115-style
transaction-budgeted `poll()` or SHT3x-style `pollJob()` lifecycle. The CLI's
`stop` command stops its sampling loop without I2C. It does not cancel an
already started hardware conversion. See [integration.md](integration.md)
and the public header for timing and partial-effect contracts.

Health uses `UNINIT`, `READY`, `DEGRADED`, `OFFLINE`, saturating counters,
consecutive failures, timestamps and the last transport error. Like OPT4001,
TMP1x2 counts individual tracked transport outcomes. Configuration trust is
separate; validation/precondition errors and not-ready results are not transport
failures. Probes and raw APIs bypass driver health. The adapter's bus counters
include diagnostic traffic. OFFLINE is passive and does not block an explicit
retry. SHT3x additionally tracks completed logical operations and protocol
failures; its counters are therefore not numerically interchangeable.

## CLI and platform compatibility

The shared TMP1x2 command processor gives the Arduino and native-IDF examples
the same command parsing, help, aliases, colors and diagnostic semantics.
Platform/framework version strings and observed transport results naturally vary.

The terminal presentation matches OPT4001's concrete help format: cyan
`=== TMP1x2 CLI Help ===`, green section labels, cyan command text left-aligned
in a 32-character minimum-width column, plain descriptions separated by ` - `,
and prompt `> `. Sections are `[Common]`, `[Data]`, `[Configuration]`,
`[Registers]` and `[Diagnostics]`. Used ANSI sequences are reset `ESC[0m`,
red 31, green 32, yellow 33, cyan 36 and gray 90. OPT4001 also defines blue 34
for debug logs; TMP1x2 has no corresponding debug-log command. `color off`
disables styling. Status messages color their severity tag; health states,
success rates and sample values have their own value coloring. Success-rate
thresholds match OPT4001: green at 99.9% or above, yellow at 80% or above,
otherwise red; no attempts produces gray `n/a`.

Common aliases include `help/?`, `version/ver`, `init/begin`, `drv/health`,
`cfg/settings` and `reg/rreg`, alongside scan, probe, recover, end, cached
samples, finite watch/stress, stop and raw register diagnostics. Sensor-specific
commands describe actual TMP capabilities. There is no chip-ID or CRC command.
Command coverage is device-specific, so it is not a byte-for-byte copy of every
sibling's CLI. Within this library, both frameworks use the same implementation.

Native IDF examples use `app_main`, `driver/i2c_master.h`, `esp_timer` and task
timing with no Arduino compatibility layer. Builds follow the sibling pattern:
`library.json` as version source, generated metadata, a C++17 component,
PlatformIO S2/S3 Arduino environments and native tests, the Windows
`scripts/pio.cmd` wrapper, and framework-boundary checks. See
[validation.md](validation.md) for checks actually run. Mock tests, SDK-header
compilation, complete firmware links and physical validation are separate results.
