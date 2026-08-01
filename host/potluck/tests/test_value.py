"""The host's Value and Reading -- ARCHITECTURE.md section 4 rule 2.

Section 4 rule 2 is a *type* guarantee, not a convention, on both sides of the link. On the firmware
side that is enforced by C++ having no accessor that returns the number alone. Here it has to be
enforced deliberately, because Python will happily invent a conversion, so these tests assert the
absence of things as much as the presence of them.

The one banned act, in the section's own words, is "handing back old data *unmarked*". Every test
below is a way that could happen.
"""

from __future__ import annotations

import math
import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from potluck.value import (
    VALUE_BYTES_MAX,
    NsError,
    Quality,
    Reading,
    StalenessPolicy,
    Unit,
    Value,
    ValueType,
    quality_has_value,
    unit_suffix,
    value_type_size,
)


class TestValue(unittest.TestCase):
    def test_every_constructor_round_trips_bit_exactly(self):
        cases = [
            (Value.of_bool(True), True),
            (Value.of_bool(False), False),
            (Value.of_i32(-2147483648), -2147483648),
            (Value.of_i32(2147483647), 2147483647),
            (Value.of_u32(0), 0),
            (Value.of_u32(4294967295), 4294967295),
            (Value.of_f32(-0.5), -0.5),
            (Value.of_i64(-9223372036854775808), -9223372036854775808),
            (Value.of_u64(18446744073709551615), 18446744073709551615),
            (Value.of_f64(-2.25), -2.25),
        ]
        for v, want in cases:
            self.assertEqual(v.number(), want, f"{ValueType.name_of(v.type)}")
            again = Value.from_wire(*v.to_wire()[:2], v.to_wire()[2])
            self.assertEqual(again.raw, v.raw)
            self.assertEqual(again.type, v.type)

    def test_a_float32_keeps_float32_precision_not_float64(self):
        # 0.1 is not representable in binary32. The stored bits must be the f32 ones, so a value that
        # travelled as f32 reads back as the f32 it was -- not as a float64 that looks tidier.
        v = Value.of_f32(0.1)
        self.assertEqual(v.raw, struct.pack("<f", 0.1))
        self.assertNotEqual(v.number(), 0.1)
        self.assertAlmostEqual(v.number(), 0.1, places=6)

    def test_a_truncated_value_returns_none_rather_than_zero_padding(self):
        # A short raw is a wire fault. Padding it would turn a fault into a plausible reading, which
        # is the same class of mistake as serving stale data unmarked.
        v = Value(int(ValueType.U32), b"\x01\x02")
        self.assertIsNone(v.number())
        self.assertIn("truncated", v.format())

    def test_a_type_is_never_reinterpreted(self):
        v = Value.of_f32(1.0)
        # 0x3F800000 read as u32 would be 1065353216 -- a plausible number and a real bug. The
        # accessor is type-faithful, so the f32 bits come back as an f32.
        self.assertEqual(v.number(), 1.0)
        self.assertEqual(v.type, int(ValueType.F32))

    def test_none_and_bytes_have_no_fixed_width(self):
        self.assertEqual(value_type_size(int(ValueType.NONE)), 0)
        self.assertEqual(value_type_size(int(ValueType.BYTES)), 0)
        self.assertEqual(value_type_size(int(ValueType.F64)), 8)

    def test_bytes_beyond_the_cap_are_refused(self):
        with self.assertRaises(ValueError):
            Value.of_bytes(b"x" * (VALUE_BYTES_MAX + 1))
        ok = Value.of_bytes(b"x" * VALUE_BYTES_MAX)
        self.assertEqual(ok.len, VALUE_BYTES_MAX)

    def test_to_wire_always_pads_to_eight_bytes(self):
        # The wire field is fixed width. A short raw here would shift every field after it.
        for v in (Value.of_bool(True), Value.of_i32(1), Value.of_f64(1.0), Value()):
            _t, _l, raw = v.to_wire()
            self.assertEqual(len(raw), VALUE_BYTES_MAX, ValueType.name_of(v.type))

    def test_an_unknown_type_is_named_not_guessed(self):
        v = Value(200, b"\x01")
        self.assertIn("200", ValueType.name_of(200))
        self.assertIsNone(v.number())


class TestQuality(unittest.TestCase):
    def test_stale_counts_as_having_a_value(self):
        # The whole point of the section: "the value is *still delivered*" past its bound, because
        # "an estimator predicting through a dropout legitimately wants the last measurement and its
        # exact age".
        self.assertTrue(quality_has_value(int(Quality.GOOD)))
        self.assertTrue(quality_has_value(int(Quality.STALE)))
        self.assertFalse(quality_has_value(int(Quality.UNAVAILABLE)))
        self.assertFalse(quality_has_value(int(Quality.NO_DATA)))
        self.assertFalse(quality_has_value(int(Quality.FAULTY)))

    def test_no_data_is_distinct_from_unavailable(self):
        # "The path exists but has never been written" and "the owning node is dead" are different
        # facts and lead to different actions. Collapsing them loses a diagnosis.
        self.assertNotEqual(int(Quality.NO_DATA), int(Quality.UNAVAILABLE))


class TestReading(unittest.TestCase):
    def _good(self) -> Reading:
        return Reading(value=Value.of_f32(21.5), unit=int(Unit.CELSIUS), timestamp_ms=1000,
                       age_ms=12, latency_class=2, quality=int(Quality.GOOD))

    def test_a_reading_refuses_to_become_a_float(self):
        # `float(x) = read(path)` losing the age is the exact failure the section exists to prevent.
        with self.assertRaises(TypeError):
            float(self._good())

    def test_the_number_never_comes_without_its_quality(self):
        n, q = self._good().number()
        self.assertEqual(n, 21.5)
        self.assertEqual(q, int(Quality.GOOD))

    def test_a_stale_reading_still_hands_back_the_number(self):
        r = self._good()
        r.quality = int(Quality.STALE)
        r.age_ms = 90_000
        n, q = r.number()
        self.assertEqual(n, 21.5)
        self.assertEqual(q, int(Quality.STALE))
        self.assertTrue(r.usable())
        # And the age must be visible in the rendered form, because that is what a human reads.
        self.assertIn("90000", r.format())

    def test_an_unavailable_reading_hands_back_no_number(self):
        r = self._good()
        r.quality = int(Quality.UNAVAILABLE)
        n, q = r.number()
        self.assertIsNone(n)
        self.assertEqual(q, int(Quality.UNAVAILABLE))
        self.assertFalse(r.usable())

    def test_the_formatted_form_always_carries_the_whole_tuple(self):
        text = self._good().format()
        for part in ("21.5", "C", "GOOD", "age", "12", "ts", "1000", "L2"):
            self.assertIn(part, text, f"{part!r} missing from {text!r}")

    def test_even_an_unusable_reading_names_its_age_and_class(self):
        r = Reading(quality=int(Quality.NO_DATA), age_ms=7, latency_class=4)
        text = r.format()
        self.assertIn("NO_DATA", text)
        self.assertIn("age", text)
        self.assertIn("L4", text)


class TestUnits(unittest.TestCase):
    def test_dimensionless_units_render_without_a_suffix(self):
        # "42 NONE" is worse than "42"; "42 COUNT" is noise.
        self.assertEqual(unit_suffix(int(Unit.NONE)), "")
        self.assertEqual(unit_suffix(int(Unit.COUNT)), "")
        self.assertEqual(unit_suffix(int(Unit.RATIO)), "")

    def test_physical_units_render_with_one(self):
        self.assertEqual(unit_suffix(int(Unit.CELSIUS)), "C")
        self.assertEqual(unit_suffix(int(Unit.MILLIMETRE)), "mm")
        self.assertEqual(unit_suffix(int(Unit.METRE_PER_SECOND2)), "m/s2")

    def test_a_millimetre_is_not_a_metre(self):
        # Section 4: a unit is part of the type, so these are different resources and the toolchain
        # must be able to say so.
        self.assertNotEqual(int(Unit.METRE), int(Unit.MILLIMETRE))

    def test_an_unknown_unit_is_named_not_dropped(self):
        self.assertIn("999", unit_suffix(999))


class TestNsError(unittest.TestCase):
    def test_the_refusals_are_distinct(self):
        # A write refused as NOT_WRITABLE, WRONG_OWNER or TYPE_MISMATCH is three different
        # conversations. Collapsing them to one failure loses the one that says the manifest is wrong.
        codes = {int(e) for e in NsError}
        self.assertEqual(len(codes), len(list(NsError)))
        self.assertEqual(int(NsError.OK), 0)

    def test_an_unknown_code_is_named_not_guessed(self):
        self.assertIn("99", NsError.name_of(99))


if __name__ == "__main__":
    unittest.main(verbosity=2)
