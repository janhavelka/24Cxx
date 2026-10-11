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
It finishes with another independent full-array comparison, including in the
read-only campaign, and records heap/console telemetry where available.
It does not program EEPROM or change WP, reset the bus, upload firmware or
deliberately pulse DTR/RTS for a reset. Control-line states are set before opening
the port: DTR defaults to zero (`--dtr 1` for Arduino S2), RTS stays zero. Drivers
or USB-UART hardware may still glitch these lines during open; check the board.
Current reads are skipped only when the adapter
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

## Timed read-only soak

After a successful finite campaign, `--soak-hours` (or `--soak-seconds`) starts
a timed read-only phase. Maximum duration is seven days; `--soak-interval`
sets the positive pause between cycles, default one second. Preview first:

```powershell
python tools/hil_runner.py --dry-run --model zetta --address 0x50 --out build-hil --soak-hours 24
python tools/hil_runner.py --port COM5 --model zetta --address 0x50 --out build-hil --soak-hours 24 --board ESP32-S3 --firmware YOUR_FLASHED_COMMIT
```

Each cycle compares a rotating up-to-256-byte read with `array-before.bin`,
checks a whole-array CRC against that same preimage, and samples idle job state,
health, heap and console. Newly tracked bus failures, content mismatch, console
loss or counter regression stop the run. Optional heap/console data are skipped
only on an explicit `UNSUPPORTED` reply; disappearing supported telemetry fails.
Heap samples record initial, lowest and last free space plus minimum-free and
largest-block values; they do not automatically classify every allocation as a
leak. A stall counter is recorded separately from actual dropped bytes.

The soak phase never writes, even if the preceding finite campaign was authorized
with `--allow-write`. There is one in-flight command, no automatic reconnect,
reset, replay or post-failure cleanup. A cycle admitted before the duration ends
finishes under the per-command deadlines; actual elapsed time and overrun are
reported. Choose `--long-timeout` for the fitted capacity and serial throughput.

`report.json` contains fixed-size aggregates (cycles, bytes, command latency,
heap, console and failure evidence) and is atomically checkpointed at startup,
after a cycle when 30 seconds have elapsed since the last checkpoint, and on exit.
A killed process may leave
`RUNNING`; it is not a pass. `soak-events.jsonl` and `soak-transcript.txt` each
retain current and previous 8 MiB segments, bounding soak logs to 32 MiB total.
Rotation deliberately discards older detailed records; the finite campaign,
preimages and aggregate report remain separate. Disk/capture/close errors make
the run inconclusive unless a failure was already established, in which case
`FAIL` is preserved. These offline-tested reporting rules are not hardware results.

## USB and console settings

| Example | Runtime console | Host control lines | Settings |
| --- | --- | --- | --- |
| Arduino S3 | Direct IDF USB Serial/JTAG driver, native USB connector | DTR=0, RTS=0 | USB mode 1, automatic CDC-on-boot disabled; 1024-byte RX/TX driver buffers |
| Arduino S2 | Explicitly initialized TinyUSB CDC, native USB connector | DTR=1, RTS=0 (`--dtr 1`) | USB mode 0, CDC-on-boot disabled; 1024-byte RX, zero TX wait, runtime USB reboot triggers disabled |
| Native IDF S3 | Direct USB Serial/JTAG driver, native USB connector | DTR=0, RTS=0 | 1024-byte RX/TX driver buffers; primary and secondary SDK consoles disabled |
| Native IDF S2 | UART0 via a USB-UART adapter, not the native USB connector | DTR=0, RTS=0 | 115200 8N1, no flow control; GPIO43 TX / GPIO44 RX by default, 1024-byte RX, direct FIFO TX |

Override native S2 pins through `EEPROM24CXX_SERIAL_TX` and
`EEPROM24CXX_SERIAL_RX` in `BoardConfig.h`/build defines. The native IDF defaults
disable power management and tickless idle; the application does not enter sleep.
Do not enable light/deep sleep during a connected USB qualification run. Existing
generated sdkconfig files retain old choices: regenerate those generated files
or set both SDK console channels to NONE. Build guards reject competing console
owners. Arduino S2 no longer treats a 1200-baud/control-line sequence as a reboot
request; subsequent flashing may require the board's manual BOOT/RESET sequence.

The shared output ring retains ordered data when the host pauses and records
queue pressure. At an 8 KiB reserve threshold it pauses command intake and
automatic bus scheduling, while tWR and whole-job deadlines still advance.
Zero-wait driver calls keep output backpressure out of the EEPROM owner loop;
`write-timeouts` counts episodes with at least one second without output progress.
An SDK accepting bytes is not proof that the terminal received them. Native S2
UART cannot detect whether the USB-UART host is present. S3 connection telemetry
observes USB host activity, not whether a terminal has the port open.

TinyUSB and UART expose receive overflow events, which invalidate queued input
before accepting another line. Native UART also treats a full event queue as
possible loss and rechecks errors before submitting a line. S3's public USB driver exposes no RX overflow
counter; `rx-loss-observable=no` reports that limitation explicitly. Zero reported
overflow on that backend cannot establish zero RX loss. Send one bounded command
at a time and wait for correlated completion; do not flood or paste batches of
programming commands. The runner follows this rule.

For a physical long-run qualification:

1. Use stable board power and a known data cable, record the USB topology and
   port, and close every other serial monitor. Record the host's suspend and USB
   power-saving settings; keep it awake for the planned run. The runner does not
   modify OS power policy.
2. Run a finite read-only campaign and inspect `console`, `heap`, `detect` and
   `help tests`. Confirm the expected backend, geometry, bank ACKs and no loss.
3. Run a 24-hour read-only soak, then inspect baseline integrity, heap trend,
   latency, loss counters, resets, completed cycles and actual duration. Repeat
   on each intended framework/board, bus speed and hub/cable setup.
4. Separately pause host reads during a long read and confirm ordered continuation
   and bounded owner activity. Test disconnect/reconnect and control-line changes
   while observing power, resets and a bus trace. A disconnected runner must stop;
   resuming qualification requires a new operator-started campaign.
5. With an external backup and disposable scratch region, separately test a
   disconnect after physical programming starts. Check retained evidence and
   tWR, then inspect contents before explicitly choosing restoration. Do not
   interpret a surviving USB session as power-loss or endurance qualification.

The transport choices were checked against the installed Arduino 3.3.11 and IDF
sources, including the [IDF USB driver contract](https://github.com/espressif/esp-idf/blob/v5.3.2/components/esp_driver_usb_serial_jtag/include/driver/usb_serial_jtag.h)
and [SDK console configuration](https://github.com/espressif/esp-idf/blob/v5.3.2/components/esp_system/Kconfig).

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
