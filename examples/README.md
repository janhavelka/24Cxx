# Diagnostic examples

Both frameworks run `common/Eeprom24CxxCli.cpp`. Commands, aliases, help layout,
ANSI colors, parsers and operation results are identical. The application owns
the bus; the core sees only typed callbacks. Configure SDA/SCL and speed in
`common/BoardConfig.h` or its documented build defines before connecting hardware.

The default model is Zetta ZD24C02B at strap address 0x50. Startup binds and checks
presence without programming memory. An ACK cannot identify the manufacturer,
capacity or geometry, and several addresses may be banks of one EEPROM.

## Build

From the repository root, with the existing managed PlatformIO installation:

```powershell
.\scripts\pio.cmd run -e esp32s3dev -e esp32s2dev
.\scripts\pio.cmd run --project-dir examples/esp_idf/basic -e esp32s3 -e esp32s2
```

The first command builds Arduino examples; the second builds native ESP-IDF.
Both configurations use a 4 MB firmware flash layout.
The native example also supports `idf.py` from `esp_idf/basic`. Both use 115200
baud and the configured board console. Select the right pins and console for
your actual hardware.

The Arduino loop is the only owner of Wire and the driver. In native IDF,
`app_main` alone owns the driver and I2C; an input task only queues characters.
IDF pre-creates handles for 0x50 through 0x57 so bank-address changes do not
allocate during transfers. No Arduino compatibility layer is involved.

Both adapters provide the driver's clock hook and the CLI platform clock in
the same domain. Every CLI tick performs at most one core transaction; scanning
also checks one address per tick. Commands and buffers are fixed-size, malformed
or overlong input is rejected, and active jobs retain their input buffers.

## Read-only session

```text
help
color off
model
settings
discover
read 0 32
dump 240 16
text 0 64
strings
crc 0 256
health
selftest
stress 20
stop
```

`read`/`dump`/`hexdump` print a hex+ASCII view, default 16 bytes. `text` prints
escaped ASCII, default 64 bytes, so EEPROM contents cannot inject terminal escape
sequences. Both accept any nonempty range within the configured capacity.
`strings [addr N [minLen]]` finds printable ASCII runs, including those crossing
bank or buffer boundaries; no arguments scans the whole configured chip. The
minimum length defaults to 4 and accepts 1..64. `crc <addr> <N>` computes
CRC32/ISO-HDLC (reflected polynomial `0xEDB88320`, initial/final XOR `0xFFFFFFFF`).
Cancelled or failed CRC jobs never report a full-range checksum.

These views use a fixed 256-byte buffer and advance with at most one physical
transaction per tick. Admission is bus-silent; `progress` shows completed bytes
across the entire requested range, and `stop`/`cancel` can interrupt the work
between transactions. Failed reads expose only the successfully completed prefix.
Other commands that access the bus are rejected while a view is active; cached
diagnostics remain available.

`selftest`/`selfcheck` read the first 16 bytes (or the entire chip if its configured
capacity is smaller). A PASS demonstrates access only, without identifying the
chip, verifying capacity, testing WP or exercising programming. `stress [N]`
performs 1..10000 read-only rounds, default 100, with ten milliseconds between
rounds; `verbose on` enables each round's transfer report. Failures are always
reported and count toward the finite limit. These diagnostics never consume
EEPROM write endurance.

`settings`, `health`, `progress`, `diag`, `variants`, `size` and version/help
commands do not touch I2C. `heap` reports application heap telemetry when the
adapter provides it; both ESP32 adapters do. Scans and explicit probe affect
adapter counters separately from tracked health. Health prints binding
separately from READY/DEGRADED/OFFLINE: a bound driver permits explicit I/O even
after transport errors. `diag` is a cached report; it does not run a selftest.

Change the model/strap address while ended, then bind or begin again:

```text
end
model zetta
addr 0x50
begin
```

`model`/`variants` list supported profiles with their capacity, page size,
pointer width, bank mapping and write time. `size` reports configured capacity.
Selecting a name does not validate
physical identity. The base address must leave the selected model's bank bits
clear. Use the core's custom geometry API for other confirmed layouts.
`settings` distinguishes staged geometry from the active binding and includes
transport limits, ACK-poll availability, clock availability and the offline
threshold. `addr`, `model` and `timeout` without arguments are cached queries,
available even while an operation runs.

## Input and diagnostic contract

Commands are lowercase. Integers accept decimal or explicit `0x` hexadecimal;
leading zeroes remain decimal. Complete tokens must parse within their allowed
range; signs, overflow, trailing characters, missing/extra arguments, control
characters and non-ASCII input are rejected before mutation. Serial lines and
direct `processCommand()` calls follow the same character restrictions. Lines
longer than 255 characters are discarded as a whole. Backspace edits an ordinary
serial line but cannot recover an overflowed or invalid line.

Help uses the sibling cyan title/commands, green section labels, 32-character
command column and `> ` prompt. `color off` removes ANSI codes. `verbose` and
`color` accept only `0`, `1`, `off` or `on` when setting a value.

The common aliases are `help`/`?`, `version`/`ver`, `init`/`begin`,
`end`/`unbind`, `cfg`/`settings`/`snapshot`, `drv`/`health`/`state`/`online`,
`progress`/`status` and `stop`/`cancel`. `bind` performs no transaction;
`init`/`begin` and `recover` perform one tracked presence check. `probe` performs
an untracked presence check. `scan` checks 0x08..0x77; `discover` checks
0x50..0x57, one address per tick. Address aliases can belong to a single chip.

Memory transfer output includes symbolic kind/state/status, structured error
detail, request ID, requested/accepted/completed/verified counts, aggregate
commit evidence and the last chunk's evidence. A verification attempt reports
its separate status and match flag. `progress` retains the last terminal result
after it is consumed from the driver.
For a read-only view spanning several core requests, the first request ID is
retained as the CLI range ID in all progress and terminal reports, including
failures and cancellation between buffers.

## Explicit programming

The following is a syntax example for a region you intend to overwrite:

```text
wverify 16 0x11 0x22 0x33
verify 16 0x11 0x22 0x33
read 16 3
```

`write`/`writebyte` report transport acceptance and write-cycle completion;
`wverify` also reads back the requested range. `fill <addr> <value> <len>` and
`fillverify` apply the equivalent operation over a range without allocating a
range-sized buffer. Input numbers are decimal or `0x` hexadecimal.

The driver splits programming at page and bank boundaries. Completion prints
accepted/completed/verified byte counts and commit evidence. WP may suppress
storage even when a write ACKs, so an unverified successful write is not proof
that data changed. A verification mismatch reports the first observed differing
byte without assuming the cause.

`stop`/`cancel` stops future work; `end` also releases the binding. A page already
sent to the EEPROM can still be programming. The physical-cycle barrier remains
until it settles. No command automatically replays uncertain writes or resets
the shared bus. After a fault, reconcile the indicated region by readback before
choosing another write.

`recover` is a presence check, not restoration of old EEPROM contents. Device
records, checksums, backups and transactional application storage belong to the
application above this driver.

See [validation results](../docs/validation.md) for actual build coverage and
[hardware validation](../docs/hardware-validation.md) for the remaining bench work.
