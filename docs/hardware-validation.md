# Hardware validation

No board has been flashed and no physical EEPROM result is claimed. Host tests
use a modeled EEPROM and cannot establish voltage margins, real write timing,
WP behavior, endurance or recovery after power interruption.

## Serial qualification runner

`tools/hil_runner.py` runs the shared Arduino/native-IDF CLI with bounded serial
deadlines and records a transcript plus a machine-readable `report.json` in a
new output directory. It requires an explicit model and strapped address and
checks these against the active firmware configuration; it never identifies an
unknown chip by writing. Review the procedure without opening a port:

```powershell
python tools/hil_runner.py --dry-run --model zetta --address 0x50 --out build-hil
```

For a live run, install `pyserial` in the host Python environment and flash the
appropriate example separately. Close other serial monitors, confirm the
configured model/pins, and leave the CLI idle with `job auto 1` and `deadline 0`.
Substitute the actual port and board metadata:

```powershell
python tools/hil_runner.py --port COM5 --model zetta --address 0x50 --out build-hil --board ESP32-S3 --chip ZD24C02B-MAGMT --voltage 3.3 --bus-hz 100000 --firmware YOUR_FLASHED_COMMIT
```

The default campaign reads the full array, saves `array-before.bin`, compares
firmware CRC/selftest results against those independently captured bytes, and
exercises discovery, presence, current-address access, read stress and watch.
It does not program EEPROM or change WP, reset the bus, upload firmware or
deliberately toggle DTR/RTS. Current reads are skipped only when the adapter
explicitly reports that capability unavailable. Hardware and firmware metadata
are operator records; the printed library version alone does not certify the
flashed source revision or physical part marking.

Programming requires a separate explicit opt-in and a scratch window of
1..256 bytes inside the configured array. Choose disposable contents, make WP
writable explicitly, and retain an independent programmer dump. This example
crosses several Zetta eight-byte page boundaries:

```powershell
python tools/hil_runner.py --port COM5 --model zetta --address 0x50 --out build-hil --allow-write --scratch-address 7 --scratch-length 64 --scratch-rounds 3
```

Before the first write, the runner verifies the scratch preimage against the
full-array capture and flushes `scratch-preimage.bin` and its metadata to disk.
It runs `rw_suite`, `xfer_demo`, `stress_mix`, `randbench` and `typed_demo`,
requiring every named check, expected operation count, correlated completion
and verified restoration. Typed testing requires at least 28 authorized bytes;
smaller windows explicitly report it as skipped. Each suite is followed by an
independent scratch comparison; a final full-array comparison detects changes
outside the selected window. The default random/mixed repetition count is
three; increase `--scratch-rounds` deliberately because every write consumes
endurance. `--long-timeout` bounds each complete asynchronous host command and
must suit the fitted geometry, serial speed and chosen repetition counts.

Any failure stops the campaign without retry or automatic restoration. A
truncated response, missing completion, reset or serial failure is
`INCONCLUSIVE`; valid firmware evidence reporting failure is `FAIL`. `PASS`
means the recorded serial procedure met its checks. It does not establish
electrical timing, retention, endurance or power-cut behavior. Dry runs are
`NOT_RUN`, and reports retain skipped/not-run procedures. Exit codes are zero
for PASS/NOT_RUN, one for FAIL and two for INCONCLUSIVE.

After an interrupted scratch procedure, inspect `job`, `scratch`, `health` and
the affected bytes before explicitly choosing `restore confirm`. Retained RAM
backup is lost on reset; the saved external preimage remains available for
operator-directed recovery. Never send further commands while another tool
owns the serial port or the previous line may be only partly transmitted.

## Manual scheduler and fault procedures

Run these on the same disposable region with an external backup. Capture
terminal results and physical bus traces together; these deliberately failing
procedures are separate from the runner's successful campaign.

| Procedure | Commands/actions | Required observation |
| --- | --- | --- |
| Admission and one-step scheduling | `job auto 0`, `xfer_reset`, `read 0 16`, `job step 0`, `xfer_assert 0`, `job step 1`, `job`, then `job auto 1` | Admission/zero-budget step issue no transaction; each positive step issues at most one. The final session ID matches admission. |
| Physical write wait after cancellation | Start a confirmed scratch suite with automatic polling off; step through the backup and first program transaction, then `job cancel` | No later programming is scheduled. Scratch backup remains; pending tWR still blocks new bus work until settled. Then explicitly `restore confirm` and resume polling. |
| Owner timeout and full-command deadline | `deadline 1`, `job auto 0`, admit a read, allow time to pass; later restore `deadline 0`/`job auto 1`. Also exercise `job timeout` during a scratch program | CLI reports TIMED_OUT, with separate physical transfer evidence. No implicit replay/restoration; retain backup after possible writes. A callback exceeding the deadline cannot be interrupted. |
| Timed fallback and chunk limits | While idle: `end`, `ackpoll 0`, `limits 4 3`, `writecycle 10`, `begin`; run a confirmed scratch suite | No address-only ACK polling; page and small transport boundaries remain respected. Wait is at least the configured tWR. Restore intended limits/preset/ACK selection while ended. |
| Hardware WP | Assert `wp 1` where wired, read a scratch byte, then `wverify` a different value | Acknowledgement may succeed; readback reports VERIFY_MISMATCH and old contents remain. Reconcile actual bytes before making WP writable. Restore any changed bytes from external backup. |
| Disconnect/partial transport fault | Disconnect/inject failure during a stepped read, page write and restoration in separate trials | Terminal failure preserves accepted/completed/verified counts and primary/restore status; no automatic write retry. Restore only after connection and observed state are understood. |
| Equal update | Read a page, `xfer_reset`, issue `uverify` with exactly those bytes, then `xfer_stats` | Compared/skipped counts cover the range; physical write attempts are zero. Change one chunk and confirm only it programs. |
| Pure current-address read | Addressed read to seed the pointer, then `current 1`; separately read the last Zetta byte then `current 1` | Pure SLA+R with no pointer prefix; exact Zetta array wrap, invalidation after recovery, and generic bank restrictions match the configured profile. |
| Explicit interface recovery | `iface_reset` on idle, recoverable and stuck buses | Up to nine clocks, START then STOP, bounded failure and post-STOP settling; inspect the physical lines. |

`limits` can never exceed the original adapter capacities, and `writecycle`
cannot shorten the geometry's required wait. GPIO pins and bus frequency are
application configuration; `bus` reports them without reconfiguring the shared
controller. Repeat a campaign after each intentional clock/adapter change.

## Bench coverage

1. Confirm ZD24C02B-MAGMT marking/package, A0/A1/A2 straps, WP state and supply.
   Check pin assignments against the archived exact Zetta datasheet. Use correct
   pull-ups and board-specific SDA/SCL configuration.
2. Build and flash the selected Arduino or native IDF CLI. Capture version,
   help, model, settings, discover and health. Confirm startup leaves contents
   unchanged against an independent programmer dump.
3. Read the full 256-byte device and validate pointer/repeated-START/STOP behavior
   with a logic analyzer. Probe all strapped addresses on separate test setups.
4. On an explicitly disposable region, program known data starting near an 8-byte
   page boundary and verify neighboring bytes are unchanged. Capture STOP,
   programming interval and optional ACK-poll NACK/ACK transitions.
5. Assert WP and repeat a verified write using data different from existing
   contents. Confirm a mismatch is reported even if the bus write acknowledges.
6. On each other fitted family layout, test pointer 0xFF/0x100, page boundaries,
   last valid byte and (where present) 0xFFFF/0x10000 bank transitions. Never use
   destructive alias tests to auto-detect an unknown chip containing useful data.
7. Inject disconnects and transfer failures. Check partial accepted/verified
   counts, retained results, no automatic replay, and bounded recovery. Cancel
   immediately after a page write and confirm the programming barrier remains.
8. Use a second bus peripheral to verify that EEPROM programming waits release
   the shared bus. Test actual adapter locking and timeout behavior in the target
   application. Record firmware, board, chip, voltage, pull-ups and bus speed.
9. Seed the pointer with an addressed read, then use `current` and inspect a pure
   SLA+R transaction without a pointer write. Test the exact Zetta last-byte read
   wrap and page-write pointer behavior separately from generic profiles.
10. On a disposable scratch region, run `rw_suite`, `xfer_demo`, `stress_mix`,
    `randbench` and `typed_demo` using their documented `confirm` forms. Compare
    pre/post dumps and CRC independently, inspect separate primary/restoration
    results, then interrupt programming/restoration and exercise `restore confirm`.
    Keep an external backup: the CLI's RAM backup does not survive reset/power loss.
11. Issue an equal `update` and confirm zero physical writes using `xfer_stats` or
    a logic analyzer. Change one bounded chunk and verify only that chunk programs.
    Repeat with WP high using `uverify` and observe separate comparison/readback
    evidence rather than interpreting an ACK as successful storage.
12. Where the optional WP GPIO is wired, confirm startup asserts it before output
    enable and `wp 0`/`wp 1` control the physical pin. Measure at least 1.2 us WP
    setup/hold at <=400 kHz (0.6 us at 1 MHz); the examples request 5 us settling
    on both sides of each GPIO change. Exercise `iface_reset` on idle, recoverable
    and stuck buses. Capture up to nine recovery clocks, SDA high with SCL high,
    START and the final STOP. Verify bounded failure, pointer invalidation and
    the post-STOP wait before subsequent I2C access on both frameworks.

Any power-loss, long-term retention or endurance qualification requires its own
controlled test and evidence; a successful readback is not that qualification.
