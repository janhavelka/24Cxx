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

Applications reusing a CLI instance cannot replace its setup while work,
programming/reset settling or a scratch backup still owns the old context.
Invalid replacement configuration preserves the current target and callbacks.
A successful idle replacement clears partial console input and stale results.

## Build

From the repository root, with the existing managed PlatformIO installation:

```powershell
.\scripts\pio.cmd run -e esp32s3dev -e esp32s2dev
.\scripts\pio.cmd run --project-dir examples/esp_idf/basic -e esp32s3 -e esp32s2
```

The first command builds Arduino examples; the second builds native ESP-IDF.
Both configurations use a 4 MB firmware flash layout.
The native example also supports `idf.py` from `esp_idf/basic`. Both use 115200
baud where applicable. S3 uses direct USB Serial/JTAG, Arduino S2 uses TinyUSB
CDC, and native S2 uses UART0 through a USB-UART adapter. Arduino S2 requires
DTR asserted; the other examples default to DTR/RTS deasserted. See the
[USB settings and long-run procedure](../docs/hardware-validation.md#usb-and-console-settings)
before opening a port. Existing native build directories must regenerate their
generated sdkconfig when changing the console defaults.

The Arduino loop is the only owner of Wire and the driver. In native IDF,
`app_main` alone owns the driver, I2C and console polling.
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
address uniqueness, testing WP or exercising programming. `stress [N [interval_ms]]`
performs 1..10000 read-only rounds, default 100. Each round reads the same
up-to-16-byte window twice and compares the bytes, then advances across the
configured array and wraps. The interval is 1..60000 ms, default 10, measured
after completion. `verbose on` enables transfer reports. Failures are always
reported and count toward the finite limit; a mismatch fails the CLI session
without rewriting the successful physical read evidence. Paired equality cannot
prove the original contents or detect consistent aliasing; the host runner
compares against its saved baseline. These diagnostics never consume endurance.

Stress and watch emit a `Repeat:` summary every five seconds and at completion:
target/completed/OK/failed rounds, successfully read bytes, elapsed time, minimum
and maximum round latency, next address and interval. Stress byte counts include
both reads. Cancellation reports only completed rounds and retains partial byte
evidence. Finished CLI jobs retain their final elapsed time.

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

`settings`, `health`, `progress`, `diag`, `variants`, `size`, `console` and version/help
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
while `page [addr]` reports page number, offset, remaining page/bank bytes, encoded
I2C slave address and pointer bytes. Profile lists include every legal base address.

`bus` shows the application's configured SDA, SCL, frequency and WP pin without
touching I2C. These are configured values, not measured electrical properties.
While ended, `limits <tx> <rx>` can reduce transfer sizes up to the original
adapter limits; TX includes the address-pointer bytes. `ackpoll 0` selects the
conservative timed write wait, and `ackpoll 1` re-enables the supplied probe
callback. It cannot create a probe capability that the adapter lacks.
`writecycle <ms>` can extend the preset programming wait but cannot shorten it;
zero restores the preset. The maximum explicit value is 1000 ms. These staged
settings take effect at the next successful bind/begin and cannot change while
a scratch backup needs restoration.

`timeout` bounds each physical adapter call. `deadline <ms>` instead bounds an
entire asynchronous CLI command, including all buffers, scratch stages and
automatic restoration. It defaults to zero (disabled), accepts values below
2^31 ms, requires a clock when enabled, and can change only while idle. Expiry
stops future scheduling and retains any scratch backup; it does not replay a
write or start restoration. A callback cannot be preempted: if it returns after
the deadline, its physical result remains visible while the CLI session reports
`TIMED_OUT`. Use a budget suitable for the full command, especially large arrays.

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
`help`/`help all` lists all commands. `help <command>` accepts aliases and shows
the command's syntax and group contract; `help <topic>` filters to `common`,
`detection`, `memory`, `programming`, `jobs`, `configuration`, `diagnostics` or
`tests`. `help quickstart` gives a read-only bring-up sequence. Unknown topics
are rejected. A single catalogue supplies all help views.

The common aliases are `help`/`?`, `version`/`ver`, `init`/`begin`,
`end`/`unbind`, `cfg`/`settings`/`snapshot`, `drv`/`health`/`state`/`online`,
`progress`/`status`/`job`/`result` and `stop`/`cancel`. `bind` performs no transaction;
`init`/`begin` and `recover` perform one tracked presence check. Bare `probe`
performs an untracked driver presence check. `probe <0x50..0x57>` uses the
application's address-only callback without changing binding or pointer knowledge.
`scan` checks 0x08..0x77; `discover`/`detect` checks 0x50..0x57, one address per
tick, even while unbound when the adapter supports it. Detection lists configured
bank addresses and reports tested, acknowledged, absent and failed addresses,
candidate ACKs, configured bank coverage and base-address presence. Transport
errors retain their address/status/detail instead of becoming ordinary absence.
ACKs remain candidates with unverified identity; address aliases can belong to
one chip. Scans and targeted probes cannot bypass a retained write-cycle barrier.

`console` reports the backend, readiness, connection state (`-1` is unknown),
configured baud, RX/TX counters, loss observability, drops, formatting failures,
stall episodes and output queue/high-water bytes. It is cached telemetry, not a
physical delivery acknowledgement. The example-only `ConsoleOutput` uses a
16 KiB ring with an 8 KiB response reserve, drains at most 256 bytes per loop
through zero-wait writes, and preserves partial-write ordering. Input polling
is bounded to 64 bytes and one submitted command per loop. Backpressure pauses
new command handling and automatic I2C scheduling while passive programming
waits and CLI deadlines continue. Detected RX loss invalidates pending input;
see the hardware guide for SDK observability limits. No extra console task or
allocation is added to the core library.

Memory transfer output includes symbolic kind/state/status, structured error
detail, request ID, requested/accepted/completed/verified counts, aggregate
commit evidence and the last chunk's evidence. A verification attempt reports
its separate status and match flag. `progress` retains the last terminal result
after it is consumed from the driver.
For a read-only view spanning several core requests, the first request ID is
retained as the CLI range ID in all progress and terminal reports, including
failures and cancellation between buffers.

Every admitted asynchronous command also emits `CLI accepted: command=...
session=...` and one terminal `CLI result: command=... session=... outcome=...
status=... code=... detail=...`. Session IDs describe the entire command; request
IDs describe individual core transfers. A scratch session finishes only after
restoration or a terminal failure. A prompt, successful stage, or returned
`IN_PROGRESS` status is not command completion. `result` and `job result` print
the cached session result again, explicitly on request. `hilmark <token>` echoes
`CLI marker: token=...`; tokens contain 1..32 ASCII letters, digits, `_` or `-`.
Markers delimit serial output without issuing I2C.

`job auto 0` pauses automatic owner polling. `job step 0` processes only passive
time; `job step 1` permits at most one physical callback. Neither bypasses a
programming wait. `job auto 1` resumes normal scheduling, while `job cancel` and
`job timeout` end future scheduling without bus traffic. Physical write-cycle
settling and whole-command deadlines continue even with automatic polling off.
Bare `job` reports the active command/session, polling mode and progress.

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
pending. Both ESP32 adapters wait 5 us before and after WP changes to satisfy
the Zetta setup/hold limits. Failed initialization does not enable WP control.
`iface_reset` explicitly invokes application bus recovery while idle
and settled, invalidates pointer knowledge and waits the selected maximum write
time plus one millisecond before allowing more bus work. This extra wait applies
even after a failed recovery callback because an emitted STOP can start a write
cycle. `end`/`stop` cannot bypass that wait. Unsupported adapters report
`UNSUPPORTED`; recovery is never an automatic response to an ordinary failure.

Arduino and native IDF use the same open-drain recovery: up to nine clock
pulses, check SDA high while SCL high, the datasheet-required START, then STOP
to leave the bus idle. The application detaches its controller first and
reinitializes it only after recovery succeeds. Native IDF recreates its address
handles; partial teardown preserves unreleased handles for a later retry.
Failed recovery keeps ordinary I2C disabled until explicit recovery succeeds.

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
| `rw_suite <addr> <len> confirm` | Eight stages: pattern write/read, `0x5A` fill/verify, `0xA5` fill, changed update, unchanged update with zero accepted bytes, and independent final update read. |
| `xfer_demo <addr> <len> confirm` | Pattern write/read and fill/verify, with bus-silent admission, zero-budget polling, busy admission, stale-owner rejection, retained-result and exactly-once consumption checks at each core stage. |
| `stress_mix <addr> <len> <N> confirm` | 1..100 complete cycles of byte write, independent read, fill, verify, changed update, unchanged update, presence probe, pointer seed, and current read. Unsupported current reads use an explicitly reported addressed-read fallback. |
| `randbench <addr> <len> <N> confirm` | 1..1000 deterministic changed-byte writes, then N independent random reads and a full-window comparison against a shadow image; seed `0x24C02B01` makes runs repeatable. |
| `typed_demo <addr> confirm` | Two 28-byte layouts cover every public integer/endian/signed/IEEE codec and both boolean values; each is independently read and decoded. |
| `scratch` | Cached primary/restore status, backup availability, rounds and metrics. |
| `restore confirm` | Explicitly authorize a new verified restoration attempt using the retained backup. |

Each test first reads the complete backup before any programming. A successful
primary test proceeds to restoration and readback verification as authorized by
the initial `confirm`. Reports distinguish primary test status from restoration
status, primary elapsed milliseconds (including backup), restoration elapsed
milliseconds, verified pattern bytes and physical read/write/probe counts.
`randbench` measures this bounded diagnostic flow, including EEPROM cycle waits;
its primary time excludes restoration.

Every suite checks out-of-range rejection without transfers and compares the
entire scratch window against its expected shadow image **before** restoration.
This detects collateral changes inside the window that restoring the backup
would otherwise conceal. Named `Scratch check` records, operation counters and
the completed/required round count make omitted steps visible. The host runner
also compares a full-array preimage after the campaign, covering bytes outside
the scratch region. All writes, including restoration, consume endurance.
`xfer_demo` requires adapter transfer counters (provided by both examples).
Without counters, the other suites report range rejection only and make no
claim to have measured bus silence.

`scratch` and `result` also retain the last terminal primary and restoration
core stages separately, including commit uncertainty, accepted/completed/verified
bytes, mismatch details and failed offsets. Later reads do not overwrite these
snapshots. They describe the last stage of each phase, not aggregate progress
across all pattern rounds; overall phase statuses and metrics remain separate.

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
[hardware validation](../docs/hardware-validation.md) for the serial runner,
manual fault procedures and remaining bench work.
