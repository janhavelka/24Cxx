# 24Cxx EEPROM reference archive

The requested part is **Zetta ZD24C02B-MAGMT**, a 256-byte I2C EEPROM. This archive contains the primary manufacturer documents used for the driver and a bounded survey of manufacturer source repositories. It covers the Zetta 24Cxx catalogue from 2 Kbit through 1 Mbit, plus representative Microchip, onsemi, Giantec and ROHM parts. ST and Renesas documents were also inspected online; download failures are recorded, not presented as archived PDFs. It is not an exhaustive inventory of every manufacturer, suffix, revision or Git repository.

All archived vendor source is for reference only and must remain excluded from library and example builds. The implementation does not depend on vendor SDKs.

## Exact target: ZD24C02B-MAGMT

Source: [Zetta's EEPROM catalogue](http://www.zettadevice.com/detail_10.html), [original PDF](http://www.zettadevice.com/uploads/files/2Kb/1678084063c994026ba112c8a0.pdf), [archived PDF](datasheets/zetta-zd24c02b.pdf), [searchable extraction](datasheets/zetta-zd24c02b.txt). Revision 1.1, 2023-01-02; 19 actual PDF pages (the printed page labels are inconsistent).

| Property | Required behavior / value | PDF page |
|---|---|---|
| Capacity | 2 Kbit = 256 bytes, offsets 0x00-0xFF | 1, 4 |
| Page size | **8 bytes**; crossing a page wraps within the same page | 1, 8 |
| Address | 7-bit `0x50 | (A2 << 2) | (A1 << 1) | A0` | 2, 6 |
| Memory address | One byte, transmitted after the device address | 6-9 |
| Write cycle | STOP starts internal programming, maximum 5 ms; device does not ACK while busy | 7-8, 13 |
| Read | Random read uses an address write followed by repeated START; sequential reads increment internally | 9-10 |
| Supply | Use 1.7-5.5 V from ordering and operating tables | 12-15 |
| Bus rate | 400 kHz at 1.7-5.5 V; 1 MHz at 2.5-5.5 V | 13 |
| WP | High inhibits all memory writes; low enables writes; reads still work | 2-3 |
| Address/WP inputs | Internal pull-down documented; intentionally strap for a deterministic board design | 2 |
| Temperature | Operating-characteristic tables cover -40 to +125 C | 12-13 |
| Endurance | 1 million write cycles under the stated 25 C, 3.3 V, page-mode conditions | 14 |
| Retention | 100 years, characterization condition as documented | 14 |
| Part suffix | MA = 2 x 3 mm UDFN; G = low-halogen/Pb-free; M = 1.7-5.5 V; T = tape/reel | 15 |

The eight-pin DFN diagram gives pins **1 A0, 2 A1, 3 A2, 4 GND, 5 SDA, 6 SCL, 7 WP, 8 VCC**. The [rendered pin diagram](datasheets/zetta-zd24c02b-page2.png) was visually checked. The PDF identifies the MA package in its ordering table but does not include its mechanical outline in the B-revision package drawings; do not treat another package's mechanical drawing as a verified MAGMT land pattern.

The first-page feature list claims a 1.6 V minimum, while the operating tables and M ordering code specify 1.7 V. The implementation documentation uses **1.7 V**. The device-address prose also contains a copied reference to page-address bits; the explicit bit table on page 6 assigns all three low address bits to A2/A1/A0 for this 2-Kbit part.

**ZD24C02A is different:** the earlier Zetta 02A/04A/08A/16A document specifies 16-byte pages and a 3 ms write cycle. It must not be used to justify a 16-byte page on the requested **02B**.

## Protocol, geometry and vendor differences

These EEPROMs have a byte array and an internal address pointer. Ordinary memory access has no temperature register, identity register, configuration register, WREN opcode, chip-erase opcode or software shutdown command. A scan/ACK cannot identify capacity, manufacturer, page size or write protection. Configure geometry from the exact BOM part.

| Memory operation | Bus sequence |
|---|---|
| Byte/page write | START, address+W, 1 or 2 memory-address bytes, data, STOP |
| ACK polling | START, address+W, ACK/NACK, STOP; polling is explicit and bounded |
| Random/sequential read | START, address+W, memory address, repeated START, address+R, data, master NACK on last byte, STOP |
| Current-address read | START, address+R, data, master NACK, STOP; depends on external pointer state |

A successful write transfer only proves the bus transaction was accepted. It does not prove that programming completed or that WP was low. Check readiness after the write and read back when confirmation matters. WP behavior differs: some devices accept the transaction but suppress programming; Renesas R1EX24002A instead documents a NACK on the data byte when WP is high.

Write chunks must stop at the configured page boundary and the transport's payload limit. The address prefix consumes transport buffer space. Read chunks should stop at bank boundaries as well as transport limits; do not assume every multi-address EEPROM rolls reads into the next bank. Never wrap a user request at the end of the array. After an interrupted transfer or power loss, previously transferred bytes may already have been programmed; a transport error cannot establish rollback.

The following are representative geometries, **not a universal guarantee for every similarly named part**. Conservative smaller page chunks remain usable when they divide the physical page size. Voltage, clock, endurance, write timing and address-pin decoding must still be checked for the exact part.

| Representative family | Capacity bytes | Page bytes | Word-address bytes | Bank bits in 7-bit address |
|---|---:|---:|---:|---|
| 24C01 | 128 | 8 typical; ST uses 16 | 1 | none |
| **ZD24C02B** | **256** | **8** | **1** | **none** |
| ZD24C02A / ST M24C02 / CAT24C02 / GT24C02 | 256 | 16 | 1 | none |
| ROHM BR24G02-3A | 256 | 8 | 1 | none |
| 24C04 | 512 | 16 | 1 | bit 0 = offset bit 8 |
| 24C08 | 1,024 | 16 | 1 | bits 1:0 = offset bits 9:8 |
| 24C16 | 2,048 | 16 | 1 | bits 2:0 = offset bits 10:8 |
| ZD24C32A | 4,096 | 32 | 2, MSB first | none |
| ZD24C64A | 8,192 | 32 | 2, MSB first | none |
| ZD24C128A | 16,384 | 64 | 2, MSB first | none |
| ZD24C256A / Microchip 24LC256 | 32,768 | 64 | 2, MSB first | none |
| ZD24C512A | 65,536 | 128 | 2, MSB first | none |
| ZD24C1MA / ST M24M01-R/DF | 131,072 | 256 | 2, MSB first | **bit 0 = offset bit 16** |
| Microchip 24AA/LC/FC1025 | 131,072 | 128 | 2, MSB first | **bit 2 = offset bit 16** |

The [ZD24C1MA addressing figure](datasheets/zetta-zd24c1ma-page7.png) was visually checked. Microchip 24XX1025 has an additional A2-pin wiring requirement (tie high) and cannot sequentially read across its 64-KiB halves. Its bank layout must remain separate from the Zetta layout.

ST M24M01-R/DF has E2 and E1 chip-enable inputs: valid base addresses are `0x50`, `0x52`, `0x54`, `0x56`; 7-bit address bit 1 is E1, not reserved. See [ST DocID12943 Rev.14, table 2](https://www.st.com/resource/en/datasheet/m24m01-r.pdf). Do not treat the newer configurable-address M24M01E-F as the same reviewed variant. Microchip 24LC1025 instead uses bases `0x50` through `0x53`; its physical A2-high requirement does not imply setting software address bit 2 (the bank bit) in the base address.

A generic density name also does not guarantee address-pin decoding. Some Microchip small B-series devices ignore pins that other vendors use for chip selection, so several scanned addresses can be aliases of one EEPROM. On banked devices, the base address must leave the bank-selection bits clear.

Some larger Zetta/ST variants add identification pages and irreversible lock commands. These are device-specific extensions, separate from the normal array; their presence in an archived datasheet does not mean this library implements or should probe them. FRAM, SPD, MAC-address parts, secure memories and SPI 25xx devices require their own reviewed behavior.

## Archived document inventory

Every successful download has its original URL, final URL, byte count, SHA-256 and searchable-text hash in [manifest.json](manifest.json). Actual downloaded revision identifiers are recorded there. The canonical vendor URL can later serve a newer document; use the stored hash when reproducing this review.

| Vendor | Downloaded documents |
|---|---|
| Zetta | ZD24C02B; ZD24C02A/04A/08A/16A; ZD24C32A; ZD24C64A; ZD24C128A; ZD24C256A; ZD24C512A; ZD24C1MA; EEPROM catalogue snapshot |
| Microchip | DS20001941L (24XX1025); DS20001703M (24XX16); DS20001203Y (24XX256) |
| onsemi | CAT24C01/D Rev.36 covering CAT24C02/04/08/16 |
| Giantec | GT24C02 A3 |
| ROHM | BR24G02-3A Rev.004 |

Inspected primary references whose local PDF downloads failed:

- [ST M24C01/02 datasheet](https://www.st.com/resource/en/datasheet/m24c02-f.pdf): direct host lookup/connection timed out. ST's [product page](https://www.st.com/en/memories/m24c02-w.html) confirms the 16-byte page difference.
- [ST M24M01-R/DF datasheet](https://www.st.com/resource/en/datasheet/m24m01-r.pdf), DocID12943 Rev.14, 2017-10: browser-readable PDF; direct ST downloads timed out. Table 2 verifies the named preset's E2/E1/A16 addressing.
- [Renesas R1EX24002A datasheet](https://www.renesas.com/en/document/dst/r1ex24002asas0ir1ex24002atas0i-datasheet-two-wire-serial-interface-2k-eeprom-256-word-x-8-bit?r=504006), R10DS0221EJ0200 Rev.2.00, 2013-11-07: browser-readable PDF, automated byte download returned HTTP 403.
- [Renesas R1EX24xxx control-software application note](https://www.renesas.com/en/document/apn/rx-family-rl78-family-renesas-r1ex24xxx-series-serial-eeprom-control-software-rev103?r=504006), R01AN1075EJ0103 Rev.1.03, 2016-03-31: browser-readable PDF, automated byte download returned HTTP 403.

The exact Zetta target and its normal-memory protocol are fully covered by the downloaded primary PDFs. These four unavailable local downloads are explicitly marked `unavailable` in the manifest.

## Manufacturer repository survey

[repository-inventory.json](repository-inventory.json) records exact repository names, resolved commit IDs, commit dates, tree-search scope, matched paths, selected downloads, upstream URLs and checksums. The source snapshots are:

| Repository | Selected material | License boundary |
|---|---|---|
| [ST X-CUBE-EEPRMA1](https://github.com/STMicroelectronics/X-CUBE-EEPRMA1) | `M24xx` C driver/header, licenses, evaluation command-tool documentation | M24xx BSD-3-Clause; command utility has separate ST terms |
| [ST stm32-m24256](https://github.com/STMicroelectronics/stm32-m24256) | Standalone M24256 C driver/header and license | BSD-3-Clause |
| [Microchip Harmony core](https://github.com/Microchip-MPLAB-Harmony/core) | AT24 driver/template, geometry configuration and public types | Microchip-specific software terms |
| [Microchip dsPIC 24C08 example](https://github.com/microchip-pic-avr-examples/dspic33ck-curiosity-i2c-eeprom-demo) | 24C08 read/write example, configuration and types | Microchip-specific software terms |
| [Renesas linux-bsp](https://github.com/renesas-rcar/linux-bsp) | Linux AT24 driver and EEPROM device-tree schema | GPL-2.0 notices retained |

ST's archived AT-command utility documentation specifically targets the **M95P32 SPI** board and is retained to explain an excluded search hit. It is a host/board interface, not an I2C EEPROM chip opcode list. SPI opcodes do not apply to 24Cxx. The local library CLI follows sibling project conventions rather than adopting those vendor evaluation commands.

The metadata search found five ST repositories and five Microchip example repositories matching its terms; the results include unrelated emulated EEPROM and SPI code. Harmony's zero metadata hits do not mean it lacks EEPROM code: its tree contains the archived AT24 driver. Renesas's general organization search likewise does not cover the separately named R-Car organization. Corrected ROHM organization, Zetta, Giantec and onsemi metadata queries returned no dedicated driver matches; supplemental web searches did not identify manufacturer-owned dedicated 24Cxx driver repositories for them. Earlier API rate-limit and organization-name errors remain in the inventory alongside successful follow-up queries.

This is a reproducible, bounded search of public repository metadata and selected trees, not authenticated global code search. No claim is made that every vendor repository was enumerated. Vendor-owned upstream Linux forks also include community code; ownership of the repository does not make every driver vendor-authored.

## Integrity, refresh and licenses

`SHA256SUMS` is the single integrity entry point: it covers every retained file in this directory except itself and Python bytecode caches, including PDFs, text extractions, rendered figures, source code, licenses, scripts and provenance metadata. `SOURCE-SHA256SUMS` separately covers only downloaded source snapshots.

From the repository root:

```text
python docs/reference/acquire_references.py
python docs/reference/acquire_sources.py
python docs/reference/update_checksums.py
```

The acquisition scripts require `requests` and `pypdf`; they record download failures without substituting HTML for a PDF. Re-running refreshes snapshots to the URLs/current branches in the scripts and changes the manifest; source snapshots themselves use resolved commit URLs. Preserve/review revision metadata when refreshing. The two rendered figures are derived from their recorded PDF pages using PyMuPDF at 1.7 scale.

Manufacturer PDFs, extracted text and figures retain their original copyrights and terms. Reference source files retain their original licenses; none is relicensed under this library's license. In particular, Microchip reference software includes Microchip-product restrictions and Linux code remains GPL. Do not copy those references into the platform-neutral implementation without a separate license review.

The previous TMP1x2/TI reference set is preserved separately in [docs/archive/tmp1x2-reference](../archive/tmp1x2-reference/README.md). It belongs to the earlier, corrected request and does not support any EEPROM claims.
