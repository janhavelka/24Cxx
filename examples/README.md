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
health
stress 20
stop
```

`read`/`dump` return 1 through 256 bytes, default 16. `stress` performs a finite
number of reads and never consumes EEPROM write endurance. `settings`, `health`,
`progress`, `diag` and version/help commands use cached state and do not touch I2C.
Scans and explicit probe affect adapter counters separately from tracked health.

Change the model/strap address while ended, then bind or begin again:

```text
end
model zetta
addr 0x50
begin
```

`model` lists the supported profile names. Selecting a name does not validate
physical identity. The base address must leave the selected model's bank bits
clear. Use the core's custom geometry API for other confirmed layouts.

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
