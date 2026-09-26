# Hardware validation

No board has been flashed and no physical EEPROM result is claimed. Host tests
use a modeled EEPROM and cannot establish voltage margins, real write timing,
WP behavior, endurance or recovery after power interruption.

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
    enable and `wp 0`/`wp 1` control the physical pin. Exercise `iface_reset` on an
    idle recoverable bus and a deliberately stuck bus, recording bounded recovery,
    pointer invalidation and the post-STOP wait before subsequent I2C access.

Any power-loss, long-term retention or endurance qualification requires its own
controlled test and evidence; a successful readback is not that qualification.
