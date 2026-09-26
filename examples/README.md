# Diagnostic examples

Both frameworks run `common/Eeprom24CxxCli.cpp`. Commands, aliases, help layout,
ANSI colors, parsers and operation results are identical. The application owns
the bus; the core sees only typed callbacks. Configure SDA/SCL and speed in
`common/BoardConfig.h` or its documented build defines before connecting hardware.

The default model is Zetta ZD24C02B at strap address 0x50. Startup runs the same
tracked `begin` presence check as explicit initialization, then prints health,
help and the prompt without programming memory. A failed presence check retains
the binding and reports degraded health. An ACK cannot identify the manufacturer,
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

`selftest`/`selfcheck` read the entire configured address range, crossing every
configured page/bank boundary, and report geometry, completed bytes, CRC32 and
health. The report is compact; it does not dump the entire chip. A PASS
demonstrates access only, without identifying the chip, proving capacity or
address uniqueness, testing WP or exercising programming. `stress [N]`
performs 1..10000 read-only rounds, default 100, with ten milliseconds between
rounds; `verbose on` enables each round's transfer report. Failures are always
reported and count toward the finite limit. These diagnostics never consume
EEPROM write endurance.

`watch <addr> <len> [N [interval_ms]]` repeatedly reads a selected 1..256-byte
region. Defaults are 20 rounds and 1000 ms between completions and the next
round; limits are 1..10000 rounds and 1..60000 ms. Every watch round prints its
data. `stop` cancels watch and stress without performing bus traffic.

`current`/`cur [N]` perform a true current-address read, with no address-pointer
prefix. The default length is one, maximum 256. An addressed read must first
establish a known pointer, and the requested range must remain in its current
bank. The transport must explicitly support this operation. Bind, bus recovery
and uncertain pointer effects invalidate knowledge; the CLI never guesses a
pointer from a presence ACK. `settings` reports pointer support/knowledge.

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
`init [addr]`/`begin [addr]` optionally select a valid base address, bind and
perform one tracked presence check. Invalid configurations preserve the previous
binding. `offline [0..255]` stages the passive failure threshold while ended;
zero disables OFFLINE. `geometry` and `timing` show cached settings and health,
while `page [addr]` reports page number, offset and remaining page/bank bytes.

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
`progress`/`status`/`job`/`result` and `stop`/`cancel`. `bind` performs no transaction;
`init`/`begin` and `recover` perform one tracked presence check. `probe` performs
an untracked presence check. `scan` checks 0x08..0x77; `discover` checks
0x50..0x57, one address per tick. Address aliases can belong to a single chip.
Scans cannot bypass a retained physical write-cycle barrier after cancellation.

Memory transfer output includes symbolic kind/state/status, structured error
detail, request ID, requested/accepted/completed/verified counts, aggregate
commit evidence and the last chunk's evidence. A verification attempt reports
its separate status and match flag. `progress` retains the last terminal result
after it is consumed from the driver.
For a read-only view spanning several core requests, the first request ID is
retained as the CLI range ID in all progress and terminal reports, including
failures and cancellation between buffers.

`stats` prints consumed core-job counters plus passive health. Stream buffers
and scratch stages are separate core jobs. `stats reset` clears those CLI
counters and adapter counters, while preserving driver health and error history.
`xfer_stats` prints physical attempts, reads, writes, probes, successes and
failures. `xfer_stats reset`/`xfer_reset` clear only adapter counters.
`xfer_assert <attempts> [reads writes probes]` compares exact current counts,
returning `OK` or `VERIFY_MISMATCH` without bus traffic. Reset counters before a
bounded operation to assess its physical transfers; probes include ACK polling.
Counter resets are rejected during a scratch test to preserve its metrics.

`wp` reads the application WP pin; `wp 1` protects and `wp 0` makes it writable.
These commands require a configured adapter and never infer storage permissions
from an ACK. Setting WP is rejected while work or a physical write cycle is
pending. `iface_reset` explicitly invokes application bus recovery while idle
and settled, invalidates pointer knowledge and waits the selected maximum write
time plus one millisecond before allowing more bus work. This extra wait applies
even after a failed recovery callback because an emitted STOP can start a write
cycle. `end`/`stop` cannot bypass that wait. Unsupported adapters report
`UNSUPPORTED`; recovery is never an automatic response to an ordinary failure.

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
`update <addr> <byte...>` first compares each physical page chunk and skips
unchanged chunks; `uverify` additionally verifies changed chunks. Update output
separates compared/skipped bytes from accepted/completed bytes. These commands
are still explicit mutation paths: changed chunks consume write endurance.

The driver splits programming at page and bank boundaries. Completion prints
accepted/completed/verified byte counts and commit evidence. WP may suppress
storage even when a write ACKs, so an unverified successful write is not proof
that data changed. A verification mismatch reports the first observed differing
byte without assuming the cause.

`stop`/`cancel` stops future work; `end` also releases the binding. A page already
sent to the EEPROM can still be programming. The physical-cycle barrier remains
until it settles. No command automatically replays uncertain writes or resets
the shared bus; `iface_reset` is an explicit application recovery command.
After a fault, reconcile the indicated region by readback before
choosing another write.

`recover` is a presence check, not restoration of old EEPROM contents. Device
records, checksums, backups and transactional application storage belong to the
application above this driver.

## Explicit scratch diagnostics

Scratch tests require a caller-selected region and the literal final token
`confirm`. They consume EEPROM endurance. A fixed 256-byte RAM backup preserves
the selected region while the test is running; neighboring bytes are never
included in a restoration write. The whole range must fit the selected chip.

| Command | Primary operation before restoration |
| --- | --- |
| `rw_suite <addr> <len> confirm` | Address-derived byte pattern, `0x5A` fill, `0xA5` fill, then update comparison that skips the unchanged data. Every writing stage is readback verified. |
| `xfer_demo <addr> <len> confirm` | One verified address-derived pattern write, demonstrating the same owner-polled page/bank scheduling used by normal operations. |
| `stress_mix <addr> <len> <N> confirm` | 1..100 alternating `0xA5`/`0x5A` verified whole-region rounds. |
| `randbench <addr> <len> <N> confirm` | 1..1000 deterministic pseudorandom byte writes and matching readbacks within the region; seed `0x24C02B01` makes runs repeatable. |
| `typed_demo <addr> confirm` | A 14-byte layout: little-endian `uint16` `0xA55A`, `uint32` `0x12345678`, and `uint64` `0x0123456789ABCDEF`, using public fixed-width codecs. |
| `scratch` | Cached primary/restore status, backup availability, rounds and metrics. |
| `restore confirm` | Explicitly authorize a new verified restoration attempt using the retained backup. |

Each test first reads the complete backup before any programming. A successful
primary test proceeds to restoration and readback verification as authorized by
the initial `confirm`. Reports distinguish primary test status from restoration
status, primary elapsed milliseconds (including backup), restoration elapsed
milliseconds, verified pattern bytes and physical read/write/probe counts.
`randbench` measures this bounded diagnostic flow, including EEPROM cycle waits;
its primary time excludes restoration.

A failed or cancelled programming stage **does not automatically restore or
retry**. The backup remains in RAM, the primary and restoration failures remain
separate, and `restore confirm` is required for another programming attempt.
This prevents an ambiguous write from being silently replayed. `stop`/`cancel`
and `end` perform no I2C, preserve any programming barrier, and retain a complete
backup after a potentially changed region. A backup-read failure schedules no
write. Failed restoration also retains the original backup.

While a changed-region backup is retained, target/model/settings changes, new
scratch tests and unrelated writes are rejected. Readback diagnostics remain
available. After `end`, bind the same target again and use `restore confirm`;
the restoration waits for any physical write barrier before writing. A
successful restoration clears the backup only after original bytes match by
readback. This RAM backup does not survive reboot or power loss; these examples
are diagnostic tests, not transactional persistent storage.

See [validation results](../docs/validation.md) for actual build coverage and
[hardware validation](../docs/hardware-validation.md) for the remaining bench work.
