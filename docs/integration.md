# Integration and ownership

The core is standard C++17 without platform headers, RTOS services, logging or
heap allocation. `Config` injects synchronous I2C callbacks and opaque contexts.
The application creates and owns the bus, pins, speed, WP pin, serialization,
timeouts and recovery policy. Driver instances and their physical devices must
have one serialized owner; methods are not ISR-safe or reentrant.

## Transport

`i2cWrite` transfers pointer byte(s) followed by data and STOP. `i2cWriteRead`
performs pointer write, repeated START, data read and STOP. A two-byte pointer
is big-endian. Callbacks perform exactly one terminal transaction, do not hide
retries and do not retain stack-buffer pointers. Their timeout includes acquiring
the bus lock and completing the transaction. Release the lock before returning;
never hold it across an EEPROM programming wait.

`TransportResult` contains a typed transport code, numeric detail, TX/RX completion
counts and write-effect evidence. Successful callbacks must report exact counts.
Failure RX data is discarded. If the controller cannot prove whether a failed
write reached memory, report INDETERMINATE. A generic NACK is not proof of an
address NACK or proof that no data reached the device.

Optional `i2cProbe` is an address-only SLA+W/ACK/STOP transaction with zero data
bytes. It supports EEPROM ACK polling. It must not send a dummy memory byte or
perform a read. Only a proven address NACK during a known write-cycle window is
an expected busy observation; unrelated errors remain errors.

`maxTxBytes` includes pointer overhead; `maxRxBytes` is payload capacity. The
core clamps those limits to its fixed buffers and splits writes again at the
physical EEPROM page and address-bank boundaries. It rejects overflowing ranges
before bus traffic rather than silently wrapping at the chip's capacity.

## Lifecycle and scheduling

`bind()` validates a profile and stores transport without I2C. It is not chip
identification. `begin()` is a compatibility bind plus one tracked presence
operation. `probe()` checks reachability without driver-health effects;
`recover()` performs a tracked presence check and does not reset the device,
replay a write or repair the bus.

`startRead`, `startWrite`, `startFill` and `startVerify` admit one operation without
I2C. An OK admission does not mean the transfer completed. `poll(nowMs, budget)`
performs at most the given number of physical callbacks. The owner schedules
polling and consumes the retained terminal result once with `takeResult()`.
New work cannot overwrite an active request or an unconsumed terminal result.
Caller buffers remain valid throughout an active operation; write/verify input
must remain unchanged. Zero-length requests are valid no-ops at an address up
to and including capacity.

Logical deadlines start at first poll; zero disables the logical deadline.
A nonzero deadline requires `Config::nowMs`; admission rejects it without that
hook. Deadlines are checked between callbacks, so an admitted callback can
overrun the logical deadline by its own bounded timeout. A clockless owner can
use a zero logical deadline and cancel at its own policy deadline. Pass monotonic
wrapping uint32_t milliseconds; adjacent timestamps must not jump by 2^31 ms or
more. The clock hook must use the same domain as `poll`/`tick`. Without the hook,
write waits conservatively include callback timeout bounds so a pre-transfer
timestamp cannot release a programming barrier too soon.

STOP starts the physical programming cycle. Without a probe, the driver waits
its documented maximum plus a millisecond quantization margin. With the optional
ACK probe, an active write requires an ACK; a continued NACK at or beyond the
write-cycle deadline fails the operation. Cancellation/timeout cannot undo an
already accepted page. `cancel()` and `end()` are bus-silent and preserve that
pending physical-cycle barrier; advance time through `tick()`/`poll()` before
rebinding or admitting more work. A terminal or ended operation's barrier can
expire by time alone. `end()` releases callback contexts and retains the terminal
result for consumption. Do not destroy/recreate a driver or access the same chip
through another instance to bypass a pending write cycle.

## Results and health

| Field | Meaning |
| --- | --- |
| bytesAccepted | Whole write chunks acknowledged by the transport; not proof WP allowed storage |
| bytesCompleted | Successful read prefix, or accepted write prefix whose programming wait completed |
| bytesVerified | Prefix read back equal to the requested contents |
| writeCommit | Aggregate NOT_COMMITTED, ACCEPTED, INDETERMINATE or VERIFIED effect evidence |
| lastChunkCommit | Effect evidence for the most recent physical write chunk |
| failedChunkOffset / failedChunkLength | Region associated with a failed or cancelled write |
| status / writeStatus | Overall completion versus original write result |

A failed transaction can leave an accepted prefix and an uncertain current page.
Never retry the entire request automatically. Read back after settling, reconcile
with the intended data and let the application decide whether to program again.
Verify mismatch is not proof that WP is asserted: wiring, faults and other owners
can also cause it. Verification proves the requested bytes are present, not that
a particular write was responsible for creating them.

Health is passive transport telemetry: UNINIT, READY, DEGRADED and OFFLINE.
OFFLINE never suppresses explicit owner work. Counters saturate; successful tracked
transport clears consecutive failures. Validation, cancellation, logical deadline
expiry and content mismatch are separate from physical transport failures.
Known write-busy address NACKs have their own counter. Cached settings/health do
no I2C; adapter counters include traffic excluded from driver health.

## Family and platform boundaries

Zetta ZD24C02B-MAGMT is the default: 256 bytes, 8-byte page, one pointer byte,
A2:A0 at 0x50 through 0x57, maximum tWR 5 ms. Generic profiles express documented
common/conservative layouts; they cannot identify manufacturers or package pins.
ST/onsemi 24C02 parts may use larger pages, and some Microchip parts ignore address
pins. The 1-Mbit Microchip 24LC1025 and ST M24M01-R/-DF use different bank bits and
pages; the ST profile does not cover M24M01E-F. Microchip 24LC1025 requires its
physical A2 pin tied high while the software base address leaves bank bit 2
clear. Select the exact layout or provide validated custom geometry.

The common driver covers the main EEPROM array. Vendor-specific identification,
lockable pages, security registers, proprietary reset and high-speed protocols
are outside the common API. WP GPIO control and application record formats,
CRC/journaling and power-failure recovery belong to the application.

The Arduino and native ESP-IDF examples own their bus and adapt errors to the
same typed transport. One loop/task alone runs the CLI and driver. The IDF input
task queues characters only. Neither adapter creates a second owner of the chip.
