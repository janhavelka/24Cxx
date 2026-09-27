# Chip coverage and field helpers

The exact default is Zetta ZD24C02B-MAGMT, using its
[revision 1.1 datasheet](reference/datasheets/zetta-zd24c02b.pdf).
The following covers the chip's documented memory and interface functions.
Page references are PDF pages, not the inconsistent printed labels.

| Chip function | Library/API support | Shared example CLI |
| --- | --- | --- |
| Byte, partial-page and page writes (pp. 7-8) | `startWrite`/`requestWrite`, page/bank/transport splitting; blocking `writeByte`, `writeOnce`, `write` | `write`, `wverify`, `fill`, `fillverify`, explicit scratch tests |
| ACK polling after STOP (p. 8) | Optional address-only probe; bounded write-cycle state and conservative timed fallback | All programming commands use the driver scheduler |
| Random and sequential reads (pp. 9-10) | Cooperative and synchronous addressed reads | `read`, `dump`, `text`, `strings`, `crc`, `watch`, full-array `selftest` |
| Current-address reads (p. 9) | `startCurrentRead`/`requestCurrentRead`, `readCurrentAddress`; explicit transport capability and known pointer required | `current`/`cur` after seeding a known pointer |
| Hardware write protection (pp. 2-3, 13) | Application GPIO policy and explicit setup/hold settling; verified operations detect suppressed writes | Optional `wp`, `wp 0`, `wp 1`; configured example WP starts high |
| Standby (p. 4) | Automatic after STOP/programming; no software sleep instruction | No extra command is needed |
| Protocol recovery (p. 4) | Up to nine clocks, SDA high with SCL high, then START; application adds STOP to return idle and invalidates the pointer | Explicit `iface_reset`, followed by a conservative programming wait |
| Address pins and bus rate (pp. 2, 6, 13) | Validated base address and geometry; application configures bus clock | `init [address]`, model/settings diagnostics; board clock configuration |

The chip has no identity register, register map, erase opcode or software lock.
Filling with `0xFF` is ordinary programming and consumes endurance. Other vendors'
extra security/identification features are outside this chip's protocol and
never run implicitly. Device presence does not establish its identity or geometry.

The shared ESP32 examples wait 5 us before and after WP changes, covering the
datasheet's 1.2 us setup/hold requirement at up to 400 kHz and 0.6 us at 1 MHz.
WP changes are admitted only while transfers and programming waits are idle.
Failed WP initialization leaves that control unavailable. Both frameworks use
the same electrical recovery helper; native IDF detaches/recreates its bus and
device handles explicitly so a controller-only reset cannot substitute for the
chip's required START sequence. Failed recovery disables I/O until a subsequent
successful explicit recovery, and the CLI preserves its post-recovery tWR wait.

The [detailed audit](audit-2026-09-26.md#datasheet-and-structure-recheck) records
the remaining electrical limits and inconsistencies checked against the source.

## Synchronous access and typed storage

The core provides `begin`/`init`, `bind`, `end`/`unbind`, `readByte`, `readOnce`,
`read` and `readCurrentAddress` with familiar sibling names. Direct reads perform
bounded synchronous callbacks, reject pending work/write barriers, and never
wait or secretly retry. `readOnce` requires a nonempty range fitting one bank
and RX transfer. A larger `read` can return a successful prefix before a later
failure; typed and single-byte helper outputs change only on complete success.

`BlockingMemory.h` adds optional synchronous programming and typed helpers over
the same cooperative driver. It owns neither the bus nor the driver. Supply an
advancing `Config::nowMs` clock and an application wait/yield callback, and call
the facade from the same serialized owner as the driver:

```cpp
#include <EEPROM24Cxx/BlockingMemory.h>

EEPROM24Cxx::EEPROM24Cxx device;
void applicationWait(uint32_t milliseconds, void* user); // Implement in the app.
EEPROM24Cxx::BlockingMemory memory(device, applicationWait);

EEPROM24Cxx::Status programCounter(uint32_t value) {
  uint8_t encoded[4];
  EEPROM24Cxx::memory::encodeUint32Le(value, encoded);
  return memory.update(16, encoded, sizeof(encoded), true, 1000);
}
```

Bind or begin `device` with the application callbacks before calling this
explicit programming function. The facade offers `read`, `write`, `fill`,
`verify`, `writeVerify`, `fillVerify`, `update`, byte methods, and single-chunk
`readOnce`/`writeOnce`/`verifyOnce`. Its default deadline is 1000 ms; pass a
positive deadline below 2^31 ms suited to the range. Each admitted memory call
finishes and consumes its own result before returning, releasing borrowed buffers on failure
as well as success. `lastResult()` retains accepted/completed/verified bytes and
phase statuses. Preflight failures do not replace that evidence.

Non-template execution and deadline logic live in `src/BlockingMemory.cpp`;
the public header retains declarations and typed template wrappers. Normal
CMake, PlatformIO and ESP-IDF library builds include it automatically. Custom
builds that list source files explicitly must compile both library `.cpp` files.

An application wait must return and must not re-enter the driver. The facade
terminates after 1024 consecutive returned waits without clock advancement;
neither a deadline nor this guard can preempt a blocked callback. Following a
failed write, `waitUntilReady()` can explicitly settle an idle residual barrier
without bus traffic or write replay. It rejects active/unconsumed operations.

Typed methods include `read`/`write` pairs for `Uint8`, `Bool`, `Uint16Le`,
`Uint32Le`, `Uint64Le`, their `Be` counterparts, `Int32Le`, `Int64Le`,
`FloatLe`/`Float32Le`, and `DoubleLe`/`Float64Le`. Floating-point storage requires
IEEE binary32/binary64. Methods encode defined byte layouts, not native struct
padding. Plain typed writes complete the programming wait; use explicit
verified operations when proof of stored content is required.

`MemoryHelpers.h` contains allocation-free codecs, `fitsRange`, `pageStart`,
`pageRemaining`, `bankRemaining`, and CRC-32/ISO-HDLC. Streaming CRC starts with
`0xFFFFFFFF`, calls `crc32Update` for each chunk, and XORs the final state with
`0xFFFFFFFF`. `BlockingMemory::crc32` streams a full range through fixed storage
under one overall deadline and updates its output only on complete success.
Its `lastResult()` describes the most recent read chunk; the returned Status
also accounts for the overall deadline.

## Wear-saving updates

`startUpdate`/`requestUpdate` and `BlockingMemory::update` compare each bounded
chunk before deciding whether to program it. Chunks respect page, bank, TX and
RX limits. Equal chunks are skipped; differing chunks are written once. This
reduces wear but is not an atomic record update or a guarantee against another
bus owner's changes. Applications still own serialization and power-loss policy.

- `bytesCompared` counts bytes from successful initial comparison reads,
  whether matching or different. `compareStatus` describes the most recent
  comparison attempt and is meaningful only when `comparisonAttempted` is true.
- `bytesSkipped` counts bytes in wholly matching chunks for which no write was
  issued; matching bytes within a differing chunk are programmed with that chunk.
- `bytesAccepted` counts only acknowledged programming payloads; with skipped
  chunks this is a total, not an address-prefix length.
- `bytesCompleted` includes skipped bytes and programming-completed chunks.
- `bytesVerified` counts the successful prefix of a separate full-range readback
  pass when verification was requested, including skipped chunks.

Core updates default to no post-write verification, consistent with core writes.
The convenience facade defaults to verified updates. Comparison failure ends
the operation before programming that chunk. Failed/uncertain writes are never
automatically replayed. Inspection, settling and recovery remain explicit.

## Current-address reads

Set `Config::supportsCurrentAddressRead` only when `i2cWriteRead` supports
`tx == nullptr, txLen == 0`: it must then issue a pure read, without a pointer
write. Both example adapters implement this contract. The opt-in defaults off
for compatibility with existing application adapters.

The driver requires a known pointer, established by a successful addressed
access, before admitting a current-address operation, including zero-length
requests. The exact Zetta profile
tracks read wrap at capacity and write wrap within a physical page after the
programming wait. Generic/custom profiles conservatively invalidate at uncertain
bank/array boundaries and after writes. Current-read requests cannot cross a
bank or wrap at capacity. Successful bind, end, transport faults and
pointer-changing fallback probes invalidate the pointer; a rejected bind leaves
the previous binding and pointer intact. Call `invalidateCurrentAddress()` after
external bus access or physical recovery. This also discards a pending write's
candidate pointer without discarding its write-cycle barrier.
