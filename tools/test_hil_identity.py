"""Offline firmware provenance parsing; no live serial or hardware evidence."""
from __future__ import annotations

import unittest

from hil_identity import check_identity, parse_identity


BANNER = "EEPROM24Cxx diagnostic CLI | Arduino-ESP32 3.3.11 | esp32s3 | built Oct 11 2026 09:08:07"
LIBRARY = "Library: 1.0.0 (a318ee5, 2026-10-11 09:08:01, clean)"


class IdentityTests(unittest.TestCase):
    def test_arduino_identity_retains_observed_fields(self):
        identity = parse_identity(BANNER, LIBRARY)
        self.assertEqual(identity, dict(version="1.0.0", commit="a318ee5", tree="clean",
                                       framework="Arduino-ESP32", framework_version="3.3.11",
                                       target="esp32s3", compiled_at="Oct 11 2026 09:08:07",
                                       build_timestamp="2026-10-11 09:08:01"))
        check_identity(identity, dict(version="1.0.0", commit="a318ee5", clean=True,
                                     framework="Arduino-ESP32", target="esp32s3"))

    def test_idf_single_digit_date_and_crlf(self):
        banner = BANNER.replace("Arduino-ESP32 3.3.11", "native-esp-idf v5.5.1-dirty").replace("Oct 11", "Oct  1")
        identity = parse_identity(banner + "\r\n", LIBRARY + "\n")
        self.assertEqual(identity["framework"], "native-esp-idf")
        self.assertEqual(identity["framework_version"], "v5.5.1-dirty")

    def test_host_unknown_is_preserved_without_claiming_verification(self):
        banner = BANNER.replace("Arduino-ESP32 3.3.11 | esp32s3", "unknown unknown | unknown")
        identity = parse_identity(banner, "1.0.0 (unknown, unknown, unknown)")
        self.assertEqual(identity["commit"], "unknown")
        self.assertEqual(identity["build_timestamp"], "unknown")
        check_identity(identity, {})
        check_identity(identity, dict(commit=None, target=False, clean=False))
        for expectations in ({"commit": "a318ee5"}, {"target": "esp32s3"},
                             {"framework": "Arduino-ESP32"}, {"clean": True}):
            with self.subTest(expectations=expectations), self.assertRaises(ValueError):
                check_identity(identity, expectations)

    def test_malformed_or_duplicated_records_are_rejected(self):
        for banner, library in (
            (BANNER + "\n" + BANNER, LIBRARY),
            (BANNER, LIBRARY + "\n" + LIBRARY),
            (BANNER, LIBRARY + " trailing"),
            (BANNER.replace("EEPROM24Cxx", "TMP1x2"), LIBRARY),
            (BANNER.replace(" | ", " / ", 1), LIBRARY),
            (BANNER.replace("esp32s3 |", "esp32s3 | extra |"), LIBRARY),
            (BANNER, "Library: 1.0.0"),
            (BANNER, LIBRARY.replace("a318ee5", "not-a-revision")),
            (BANNER, LIBRARY.replace("clean", "clean, dirty")),
            (BANNER.replace("Oct 11", "Oct 32"), LIBRARY),
            (BANNER.replace("09:08:07", "29:08:07"), LIBRARY),
            (BANNER, LIBRARY.replace("2026-10-11", "2026-02-30")),
        ):
            with self.subTest(banner=banner, library=library), self.assertRaises(ValueError):
                parse_identity(banner, library)

    def test_each_explicit_mismatch_is_rejected(self):
        identity = parse_identity(BANNER, LIBRARY)
        for expectations in ({"version": "1.0.1"}, {"commit": "b318ee5"},
                             {"framework": "native-esp-idf"}, {"target": "esp32s2"}):
            with self.subTest(expectations=expectations), self.assertRaises(ValueError):
                check_identity(identity, expectations)
        with self.assertRaises(ValueError):
            check_identity(parse_identity(BANNER, LIBRARY.replace("clean", "dirty")), {"clean": True})

    def test_git_prefix_comparison_preserves_abbreviation_limit(self):
        identity = parse_identity(BANNER, LIBRARY)
        check_identity(identity, {"commit": "A318EE5" + "1" * 33})
        self.assertEqual(identity["commit"], "a318ee5")
        identity = parse_identity(BANNER, LIBRARY.replace("a318ee5", "A318EE5" + "2" * 33))
        check_identity(identity, {"commit": "a318ee5"})
        with self.assertRaises(ValueError):
            check_identity(identity, {"commit": "a318ee5" + "1" * 33})
        with self.assertRaises(ValueError):
            check_identity(parse_identity(BANNER, LIBRARY.replace("a318ee5", "a318")), {"commit": "a318ee5"})

    def test_invalid_expectations_do_not_silently_disable_verification(self):
        identity = parse_identity(BANNER, LIBRARY)
        for expectations in ({"commit": "a318ee"}, {"commit": "a318ee5" + "1" * 34},
                             {"commit": "unknown"}, {"commit": "a318ee!"},
                             {"framework": "unknown"}, {"version": ""},
                             {"target": True}, {"clean": "yes"}):
            with self.subTest(expectations=expectations), self.assertRaises(ValueError):
                check_identity(identity, expectations)


if __name__ == "__main__":
    unittest.main()
