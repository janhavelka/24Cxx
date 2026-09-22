"""Re-fetch the pinned EEPROM reference archive; requires requests and pypdf.

Run from any directory. Downloads are reference-only and keep upstream licenses.
The manifest records original URLs, exact bytes and revision metadata. HTTP is
used only where the manufacturer publishes its catalogue/PDFs over HTTP.
"""
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timezone
from hashlib import sha256
from pathlib import Path
import io
import json

import requests
from pypdf import PdfReader

ROOT = Path(__file__).resolve().parent
ZETTA = "http://www.zettadevice.com"
SOURCES = [
    ("zetta-catalogue.html", ZETTA + "/detail_10.html", "Zetta", "EEPROM product catalogue"),
    ("datasheets/zetta-zd24c02b.pdf", ZETTA + "/uploads/files/2Kb/1678084063c994026ba112c8a0.pdf", "Zetta", "ZD24C02B, including MA package ordering code"),
    ("datasheets/zetta-zd24c02-04-08-16a.pdf", ZETTA + "/uploads/files/E%E6%96%B0/2~16Kb/1649400305cc6c4676b761680f.pdf", "Zetta", "ZD24C02A/04A/08A/16A"),
    ("datasheets/zetta-zd24c32a.pdf", ZETTA + "/uploads/files/E%E6%96%B0/32Kb/1649400311d69ff41f2bd2b276.pdf", "Zetta", "ZD24C32A"),
    ("datasheets/zetta-zd24c64a.pdf", ZETTA + "/upload/file/pdf/ZETTA_ZD24C64A.pdf", "Zetta", "ZD24C64A"),
    ("datasheets/zetta-zd24c128a.pdf", ZETTA + "/uploads/files/128Kb%20%E6%96%B0/1650529676c07ab37479a047b6.pdf", "Zetta", "ZD24C128A"),
    ("datasheets/zetta-zd24c256a.pdf", ZETTA + "/uploads/files/E%E6%96%B0/256Kb/1672881019c3583b0cb030d36f.pdf", "Zetta", "ZD24C256A"),
    ("datasheets/zetta-zd24c512a.pdf", ZETTA + "/uploads/files/E%E6%96%B0/512Kb/1672881029fd1b96d9de3ff94b.pdf", "Zetta", "ZD24C512A"),
    ("datasheets/zetta-zd24c1ma.pdf", ZETTA + "/uploads/files/E%E6%96%B0/1Mb/1672881007c9d6bec36b88d23a.pdf", "Zetta", "ZD24C1MA"),
    ("datasheets/st-m24c01-02.pdf", "https://www.st.com/resource/en/datasheet/m24c02-f.pdf", "STMicroelectronics", "M24C01/02: vendor page-size difference"),
    ("datasheets/st-m24m01.pdf", "https://www.st.com/resource/en/datasheet/m24m01-r.pdf", "STMicroelectronics", "M24M01-R/DF: low bank bit, E1/E2 chip enables and 256-byte pages"),
    ("datasheets/onsemi-cat24c02-04-08-16.pdf", "https://www.onsemi.com/download/data-sheet/pdf/cat24c01-d.pdf", "onsemi", "CAT24C02/04/08/16: vendor page-size difference"),
    ("datasheets/giantec-gt24c02.pdf", "https://www.giantec-semi.com/juchen1123/uploads/pdf/GT24C02_DS_Au.pdf", "Giantec", "GT24C02: vendor page-size difference"),
    ("datasheets/microchip-24lc1025.pdf", "https://ww1.microchip.com/downloads/en/DeviceDoc/20001941L.pdf", "Microchip", "24AA1025/24LC1025/24FC1025: bank-address difference"),
    ("datasheets/microchip-24lc16b.pdf", "https://ww1.microchip.com/downloads/en/DeviceDoc/20001703M.pdf", "Microchip", "24AA16/24LC16B: bank addressing"),
    ("datasheets/microchip-24lc256.pdf", "https://ww1.microchip.com/downloads/aemDocuments/documents/MPD/ProductDocuments/DataSheets/24AA256-24LC256-24FC256-256K-I2C-Serial-EEPROM-DS20001203.pdf", "Microchip", "24AA256/24LC256/24FC256: 16-bit word address"),
    ("datasheets/rohm-br24g02-3a.pdf", "https://fscdn.rohm.com/en/products/databook/datasheet/ic/memory/eeprom/br24g02-3a-e.pdf", "ROHM", "BR24G02-3A: 8-byte pages"),
    ("datasheets/renesas-r1ex24002a.pdf", "https://www.renesas.com/en/document/dst/r1ex24002asas0ir1ex24002atas0i-datasheet-two-wire-serial-interface-2k-eeprom-256-word-x-8-bit?r=504006", "Renesas", "R1EX24002A: 16-byte pages, WP data-byte NACK behavior"),
    ("datasheets/renesas-r1ex24xxx-software.pdf", "https://www.renesas.com/en/document/apn/rx-family-rl78-family-renesas-r1ex24xxx-series-serial-eeprom-control-software-rev103?r=504006", "Renesas", "R01AN1075EJ0103: serial EEPROM control software, address conversion and ACK polling"),
]


def fetch(source):
    relative, url, manufacturer, purpose = source
    entry = dict(path=relative, url=url, manufacturer=manufacturer, purpose=purpose,
                 retrieved_utc=datetime.now(timezone.utc).isoformat(),
                 license="Copyright remains with manufacturer; see document notices. Reference only.")
    try:
        response = requests.get(url, timeout=35)
        response.raise_for_status()
        data = response.content
        if relative.endswith(".pdf") and not data.startswith(b"%PDF"):
            raise ValueError("Response is not PDF")
        destination = ROOT / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(data)
        entry.update(status="archived", final_url=response.url, bytes=len(data), sha256=sha256(data).hexdigest())
        if relative.endswith(".pdf"):
            reader = PdfReader(io.BytesIO(data))
            contents = "\n\n".join(f"--- PDF page {n + 1} ---\n{page.extract_text()}" for n, page in enumerate(reader.pages))
            destination.with_suffix(".txt").write_text(contents, encoding="utf-8")
            entry["pages"] = len(reader.pages)
            entry["text_path"] = str(destination.with_suffix(".txt").relative_to(ROOT)).replace("\\", "/")
            entry["text_sha256"] = sha256(destination.with_suffix(".txt").read_bytes()).hexdigest()
        print(relative, entry["status"], flush=True)
    except Exception as error:
        entry.update(status="unavailable", error=str(error))
        print(relative, entry["status"], str(error), flush=True)
    return entry


def main():
    previous = {}
    if (ROOT / "manifest.json").exists():
        previous = {item["path"]: item for item in json.loads((ROOT / "manifest.json").read_text(encoding="utf-8")).get("artifacts", [])}
    with ThreadPoolExecutor(max_workers=6) as pool:
        records = list(pool.map(fetch, SOURCES))
    for number, item in enumerate(records):
        old = previous.get(item["path"])
        if item["status"] != "archived" and old and old.get("status") == "archived":
            # Failed refreshes preserve the previous known downloaded artifact.
            old["refresh_error"] = item["error"]
            records[number] = old
    (ROOT / "manifest.json").write_text(json.dumps(dict(scope="Manufacturer EEPROM references; not an exhaustive catalogue of every 24Cxx manufacturer or revision", artifacts=records), indent=2) + "\n", encoding="utf-8")
    import update_checksums
    update_checksums.main()


if __name__ == "__main__":
    main()
