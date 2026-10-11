"""Offline typed EEPROM transfer evidence regressions; no hardware is used."""
from __future__ import annotations

import unittest

from hil_runner import EvidenceError, transfer_evidence, validate_transfer_record


def record(kind="READ", *, requested=40, accepted=None):
    writing = kind in {"WRITE", "FILL", "UPDATE", "VERIFIED_WRITE", "VERIFIED_FILL", "VERIFIED_UPDATE"}
    verified_write = kind in {"VERIFIED_WRITE", "VERIFIED_FILL", "VERIFIED_UPDATE"}
    if accepted is None:
        accepted = requested if writing else 0
    return {
        "kind": kind, "state": "SUCCEEDED", "address": "0x00020", "requested": str(requested),
        "accepted": str(accepted), "completed": str(requested),
        "verified": str(requested if kind == "VERIFY" or verified_write else 0),
        "commit": "VERIFIED" if requested and verified_write else "ACCEPTED" if accepted else "NOT_APPLICABLE",
        "last-chunk-commit": "ACCEPTED" if accepted else "NOT_APPLICABLE", "status": "OK", "detail": "0",
        "request-id": "2147483648",
    }


class TransferEvidenceTests(unittest.TestCase):
    def assert_invalid(self, data):
        with self.assertRaises(EvidenceError):
            validate_transfer_record(data)

    def test_successful_reads_and_verification_have_no_write_effect(self):
        for kind in ("READ", "CURRENT_READ", "VERIFY"):
            data = record(kind)
            with self.subTest(kind=kind):
                validate_transfer_record(data)
                for field, value in (("accepted", "1"), ("commit", "ACCEPTED"),
                                     ("last-chunk-commit", "ACCEPTED")):
                    self.assert_invalid(dict(data, **{field: value}))
                self.assert_invalid(dict(data, verified="0" if kind == "VERIFY" else "40"))

    def test_verified_writes_require_acceptance_and_complete_verified_effect(self):
        for kind in ("VERIFIED_WRITE", "VERIFIED_FILL"):
            data = record(kind)
            with self.subTest(kind=kind):
                validate_transfer_record(data)
                for field, values in {
                    "accepted": ("0", "39", "41"), "completed": ("39",), "verified": ("0", "39", "41"),
                    "commit": ("INDETERMINATE", "ACCEPTED", "NOT_COMMITTED", "NOT_APPLICABLE", "NO_WRITE"),
                    "last-chunk-commit": ("INDETERMINATE", "VERIFIED", "NOT_COMMITTED", "NOT_APPLICABLE"),
                }.items():
                    for value in values:
                        with self.subTest(field=field, value=value):
                            self.assert_invalid(dict(data, **{field: value}))

    def test_verified_update_accepts_skipped_chunks_without_inventing_writes(self):
        for accepted in (0, 8, 40):
            data = record("VERIFIED_UPDATE", accepted=accepted)
            with self.subTest(accepted=accepted):
                validate_transfer_record(data)
                self.assert_invalid(dict(data, commit="INDETERMINATE"))
                self.assert_invalid(dict(data, commit="NOT_APPLICABLE"))
                self.assert_invalid(dict(data, verified="39"))
                wrong_last = "ACCEPTED" if accepted == 0 else "NOT_APPLICABLE"
                self.assert_invalid(dict(data, **{"last-chunk-commit": wrong_last}))
        self.assert_invalid(record("VERIFIED_UPDATE", accepted=41))

    def test_unverified_writes_and_updates_cannot_claim_readback(self):
        for kind in ("WRITE", "FILL", "UPDATE"):
            accepted_values = (0, 8, 40) if kind == "UPDATE" else (40,)
            for accepted in accepted_values:
                data = record(kind, accepted=accepted)
                with self.subTest(kind=kind, accepted=accepted):
                    validate_transfer_record(data)
                    self.assert_invalid(dict(data, verified="40"))
                    self.assert_invalid(dict(data, commit="VERIFIED"))
                    self.assert_invalid(dict(data, commit="INDETERMINATE"))
            if kind != "UPDATE":
                self.assert_invalid(record(kind, accepted=8))

    def test_zero_length_core_noops_retain_no_write_effect(self):
        for kind in ("READ", "CURRENT_READ", "VERIFY", "WRITE", "FILL", "UPDATE",
                     "VERIFIED_WRITE", "VERIFIED_FILL", "VERIFIED_UPDATE"):
            with self.subTest(kind=kind):
                data = record(kind, requested=0)
                validate_transfer_record(data)
                self.assert_invalid(dict(data, commit="VERIFIED"))
                self.assert_invalid(dict(data, **{"last-chunk-commit": "ACCEPTED"}))

    def test_schema_success_detail_and_numeric_bounds_are_required(self):
        data = record()
        for field in data:
            incomplete = dict(data)
            del incomplete[field]
            with self.subTest(missing=field):
                self.assert_invalid(incomplete)
        self.assert_invalid(dict(data, surprise="value"))
        for field, values in {
            "kind": ("NONE", "UNKNOWN"), "state": ("ACTIVE", "FAILED", "CANCELLED"),
            "status": ("IN_PROGRESS", "IO_ERROR"), "detail": ("1", "-1", "00", "0x0"),
            "request-id": ("0", "4294967296", "-1", "+1", "1.0"),
            "address": ("-1", "+32", "0x100000000", "4294967295", "bad"),
            "requested": ("4294967296", "-1", "4e1"), "completed": ("39", "4294967296"),
            "accepted": ("-1", "4294967296"), "verified": ("-1", "4294967296"),
        }.items():
            for value in values:
                with self.subTest(field=field, value=value):
                    self.assert_invalid(dict(data, **{field: value}))
        self.assert_invalid(dict(data, requested=40))
        for request_id in ("1", "2147483647", "2147483648", "4294967295"):
            validate_transfer_record(dict(data, **{"request-id": request_id}))

    def test_transfer_wrapper_keeps_request_identity_range_and_kind_checks(self):
        data = record()
        output = "Transfer: " + " ".join(f"{key}={value}" for key, value in data.items()) + "\n"
        self.assertEqual(transfer_evidence(output, 40, address=32), data)
        for kwargs in (dict(requested=39), dict(requested=40, address=31),
                       dict(requested=40, kind="CURRENT_READ")):
            with self.subTest(kwargs=kwargs), self.assertRaises(EvidenceError):
                transfer_evidence(output, **kwargs)
        for corrupt in (output + output, output.replace("detail=0", "detail=1"),
                        output.replace("request-id=2147483648", "request-id=4294967296"),
                        output.replace("commit=NOT_APPLICABLE", "commit=INDETERMINATE", 1)):
            with self.subTest(output=corrupt), self.assertRaises(EvidenceError):
                transfer_evidence(corrupt, 40)


if __name__ == "__main__":
    unittest.main()
