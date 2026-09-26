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

Current-address access requires explicit `supportsCurrentAddressRead = true`.
Then `i2cWriteRead` must also accept a null TX pointer with zero TX length and
issue a pure read. Existing adapters can leave this capability disabled. The
core admits current reads only while its pointer is known; external bus access
or recovery must call `invalidateCurrentAddress()`.

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

`init()` aliases `begin()` and `unbind()` aliases `end()`. Direct synchronous
`readByte`, `readOnce`, `read` and `readCurrentAddress` reject active work, retained
results and programming barriers. They neither wait nor retry. `readOnce` requires
a nonempty range fitting one bank and RX transaction. The optional
[blocking facade and typed helpers](field-helpers.md) provide synchronous writes
using an application-owned wait callback.

`startRead`, `startCurrentRead`, `startWrite`, `startFill`, `startUpdate` and
`startVerify` admit one operation without
I2C. An OK admission does not mean the transfer completed. `poll(nowMs, budget)`
performs at most the given number of physical callbacks. The owner schedules
polling and consumes the retained terminal result once with `takeResult()`.
New work cannot overwrite an active request or an unconsumed terminal result.
Caller buffers remain valid throughout an active operation; write/verify input
must remain unchanged. Addressed zero-length requests are valid no-ops at an
address up to and including capacity. Zero-length current reads still require
the pure-read transport capability and a known pointer.

The sibling-compatible `requestRead`, `requestWrite`, `requestFill` and
`requestVerify`, `requestCurrentRead` and `requestUpdate` names use the same
scheduler. Explicit-ID overloads accept
`1..0x7FFFFFFF`; `start*` and unqualified requests allocate upper-half IDs.
`requestVerifiedWrite` and `requestVerifiedFill` accept explicit IDs and can
span multiple physical pages. Inspect `TransferResult::requestId`, or pass the
ID to `pollTransfer`, `cancelTransfer`, `timeoutTransfer` and
`takeTransferResult`. A mismatched ID returns BUSY with
`BusyDetail::REQUEST_ID_MISMATCH` without consuming a result, advancing the
write-cycle barrier or accessing the bus. Keep explicit IDs unique while old
owner messages can still arrive. Automatic IDs survive bind/end and wrap within
their reserved half after exhausting that range; they are correlation tokens,
not globally unique identities.

Logical deadlines start at first poll; zero disables the logical deadline.
A nonzero deadline requires `Config::nowMs`; admission rejects it without that
hook. Deadlines are checked between callbacks, so an admitted callback can
overrun the logical deadline by its own bounded timeout. A clockless owner can
use a zero logical deadline and cancel at its own policy deadline. Pass monotonic
wrapping uint32_t milliseconds; adjacent timestamps must not jump by 2^31 ms or
more. The clock hook must use the same domain as `poll`/`tick`. Without the hook,
write waits conservatively include callback timeout bounds so a pre-transfer
timestamp cannot release a programming barrier too soon.

Clockless owners can also call `timeoutTransfer(requestId)` to retain a
TIMED_OUT/TIMEOUT result instead of CANCELLED. This call performs no I2C and
preserves accepted bytes and the physical write-cycle barrier. The existing
`poll`/`cancel`/`takeResult` methods remain available to a serialized owner
that does not need ID checks.

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
| requestId | Correlation token retained through terminal completion and end |
| bytesAccepted | Whole write chunks acknowledged by the transport; not proof WP allowed storage |
| bytesCompleted | Successful read/work prefix; writes count programming-completed chunks and updates also count skipped chunks |
| bytesVerified | Prefix read back equal to the requested contents |
| bytesCompared / bytesSkipped | Update comparison bytes and matching bytes that needed no programming |
| comparisonAttempted / compareStatus | Whether an update comparison ran and its latest transport outcome |
| writeCommit | Aggregate NOT_APPLICABLE, NOT_COMMITTED, ACCEPTED, INDETERMINATE or VERIFIED effect evidence |
| lastChunkCommit | Effect evidence for the most recent physical write chunk |
| failedChunkOffset / failedChunkLength | Request-relative failed read/write chunk or mismatch; cancellation/deadline uses phase progress (verification prefix during readback), with pending-write length when applicable |
| status / writeStatus | Overall completion versus the most recent physical write result |
| verificationAttempted / verifyStatus | Whether readback was attempted and its most recent transport/content outcome |

A failed transaction can leave an accepted prefix and an uncertain current page.
Never retry the entire request automatically. Read back after settling, reconcile
with the intended data and let the application decide whether to program again.
Verify mismatch is not proof that WP is asserted: wiring, faults and other owners
can also cause it. Verification proves the requested bytes are present, not that
a particular write was responsible for creating them.

An admitted nonempty write has `writeStatus == IN_PROGRESS` until its first
physical write. An update that skips every chunk changes this to OK when its
comparison phase completes, without issuing a write. A cancelled-before-start
write has NOT_APPLICABLE effect and
zero accepted bytes. `verifyStatus` is meaningful only when
`verificationAttempted` is true. A later cancellation or owner timeout does not
erase the last physical write/readback evidence; overall `status` is the
authority for logical success. A mismatch also reports its request-relative
offset in `Status::detail`.

Updates compare bounded page/bank/transport chunks and program only differing
chunks. Their `bytesAccepted` is a payload total rather than an address prefix
because earlier chunks may have been skipped. `bytesCompleted` remains a
contiguous work prefix. Verified updates perform a separate full-range readback,
including skipped chunks; initial comparisons do not increment `bytesVerified`.
An all-equal update performs no write; a successful separate verification pass
can still report VERIFIED because it proves the requested content is present.
Without that separate pass, its effect remains NOT_APPLICABLE. `bytesCompared`
counts successful comparison reads, including differing chunks; `bytesSkipped`
counts only wholly matching chunks. See the
[field helper guide](field-helpers.md) for the complete update contract.

Possibly accepted failed writes terminate without automatic replay or
automatic reconciliation reads. Consume the result, allow the write barrier to
settle, make the bus usable through application policy, and explicitly admit a
new verification request if desired. This keeps failed-write evidence available
while giving the owner control of readback timing.

Health is passive transport telemetry: UNINIT, READY, DEGRADED and OFFLINE.
OFFLINE never suppresses explicit owner work. Counters saturate; successful tracked
transport clears consecutive failures. Validation, cancellation, logical deadline
expiry and content mismatch are separate from physical transport failures.
Known write-busy address NACKs have their own counter. Cached settings/health do
no I2C; adapter counters include traffic excluded from driver health.

`state()`/`driverState()`, `getConfig()`, `getSettings()` (including the output
overload), `getSettingsSnapshot()`/`settingsSnapshot()`, and the health counter,
timestamp and last-error getters are bus-silent. `isOnline()` follows MB85RC's
binding shorthand, including DEGRADED/OFFLINE; use `state()` to assess health.
Both successful bind and end reset per-binding health. `lastError` remains the
last tracked fault after subsequent successes, while the failure streak resets.
`capacityBytes()`, `maxAddress()`, `maxWriteDataBytes()` and
`maxReadDataBytes()` return zero when unbound; snapshot geometry fields are
meaningful as a selected layout only when `bound` is true.

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
are outside the common API. WP GPIO control, record formats, journaling and
power-failure recovery belong to the application.
Optional endian/CRC codecs and WP/recovery example hooks assist that policy
without placing pins, allocation or platform delays in the core.

The Arduino and native ESP-IDF examples own their bus and adapt errors to the
same typed transport. One loop/task alone runs the CLI and driver. The IDF input
task queues characters only. Neither adapter creates a second owner of the chip.

The Wire example requires Arduino-ESP32 3.3.11 or newer and explicitly reserves
its controller for each complete callback: Wire mutex acquisition is not bounded
by its transaction timeout. It establishes buffer capacity before enabling the
adapter and restores the previous timeout after each callback. A missing or
short TX buffer is discarded without sending memory data and disables the
adapter until application reinitialization; the driver cannot repair framework
state. The native IDF adapter keeps ordinary NACKs unspecified when the SDK
cannot prove the failing byte. Only an address-only probe can establish the
address NACK used for expected EEPROM busy polling.
