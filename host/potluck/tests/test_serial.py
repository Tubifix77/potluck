"""The host and the node must frame bytes identically.

Third independent implementation of a wire-level format in this project, checked the same way as
the first two: reference vectors both sides assert, plus round-trip and resynchronisation
properties. A serial framing mismatch presents as a link that looks dead rather than wrong, which
is the hardest kind of bug to find on a bench.
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from potluck import frame as fr  # noqa: E402
from potluck.serial_framing import (  # noqa: E402
    CRC16_INIT,
    DELIMITER,
    SERIAL_FRAME_MAX,
    SerialError,
    SerialReassembler,
    cobs_decode,
    cobs_encode,
    cobs_max_encoded,
    crc16,
    read_serial_frame,
    write_serial_frame,
)


def a_frame(payload_len: int = 48) -> bytes:
    payload = bytes((i * 5 + 1) & 0xFF for i in range(payload_len))
    return fr.encode(src=0x0102, dst=0x0304, opcode=fr.Op.HEARTBEAT, lclass=3, payload=payload)


def test_crc16_matches_the_conformance_vector_the_firmware_asserts():
    # tests/test_serial_framing.cpp asserts this same number.
    assert crc16(b"123456789") == 0x29B1
    assert crc16(b"") == CRC16_INIT


def test_crc16_catches_every_single_bit_flip():
    f = a_frame()
    good = crc16(f)
    for byte in range(len(f)):
        for bit in range(8):
            m = bytearray(f)
            m[byte] ^= 1 << bit
            assert crc16(bytes(m)) != good


def test_cobs_round_trips_the_awkward_inputs():
    cases = {
        "empty": b"",
        "single zero": b"\x00",
        "all zeros": b"\x00" * 64,
        "no zeros": b"\xaa" * 64,
        "leading zero": b"\x00\x01\x02",
        "trailing zero": b"\x01\x02\x00",
        "253 non-zero": b"\x01" * 253,
        # 254 is where COBS changes gear and starts a new group.
        "254 non-zero": b"\x01" * 254,
        "255 non-zero": b"\x01" * 255,
        "510 non-zero": b"\x01" * 510,
        "every byte value": bytes(range(256)),
        "max mtu": b"\x5a" * 1470,
    }
    for name, data in cases.items():
        enc = cobs_encode(data)
        assert 0 not in enc, f"{name}: encoding contains a zero, so the delimiter is ambiguous"
        assert len(enc) <= cobs_max_encoded(len(data)), name
        assert cobs_decode(enc) == data, name


def test_cobs_rejects_malformed_input():
    assert cobs_decode(b"\x03\x11\x00\x22") is None  # zero inside the body
    assert cobs_decode(b"\x09\x11\x22") is None  # group overruns the input
    assert cobs_decode(b"") is None


def test_a_framed_pot_frame_round_trips_and_stays_parseable():
    f = a_frame()
    wire = write_serial_frame(f)
    assert wire[-1] == DELIMITER
    assert 0 not in wire[:-1]
    back = read_serial_frame(wire[:-1])
    assert back == f
    # The envelope left no trace: the recovered bytes are still a valid Potluck Frame.
    assert fr.parse(back).opcode == fr.Op.HEARTBEAT


def test_a_corrupted_frame_is_refused():
    f = a_frame()
    wire = write_serial_frame(f)
    refused = 0
    for i in range(len(wire) - 1):
        m = bytearray(wire[:-1])
        m[i] ^= 0x01
        if m[i] == 0:
            continue  # that is a delimiter, i.e. a different boundary, not corruption
        try:
            read_serial_frame(bytes(m))
        except SerialError:
            refused += 1
        else:
            raise AssertionError(f"corruption at byte {i} was not detected")
    assert refused > 0


def test_the_reassembler_recovers_frames_one_byte_at_a_time():
    stream = b"".join(write_serial_frame(a_frame(8 + i * 16)) for i in range(5))
    r = SerialReassembler()
    got = []
    for i in range(len(stream)):
        got.extend(r.feed(stream[i : i + 1]))
    assert len(got) == 5
    assert r.frames_ok == 5
    assert r.bad_crc == 0
    assert r.bytes_in == len(stream)


def test_attaching_mid_stream_resynchronises():
    stream = b"".join(write_serial_frame(a_frame(32)) for _ in range(4))
    r = SerialReassembler()
    got = list(r.feed(stream[10:]))
    assert len(got) == 3  # the truncated first frame is discarded, not delivered
    assert r.bad_crc + r.cobs_invalid == 1


def test_garbage_between_frames_does_not_lose_the_frames_around_it():
    f = write_serial_frame(a_frame(24))
    r = SerialReassembler()
    got = list(r.feed(f + b"\xde\xad\xbe\xef\x00" + f))
    assert len(got) == 2
    assert r.bad_crc + r.cobs_invalid >= 1


def test_an_overlong_run_is_dropped_and_the_stream_recovers():
    r = SerialReassembler()
    stream = b"\x41" * (SERIAL_FRAME_MAX + 200) + b"\x00" + write_serial_frame(a_frame(16))
    got = list(r.feed(stream))
    assert r.too_long == 1
    assert len(got) == 1


def test_repeated_delimiters_are_normal():
    r = SerialReassembler()
    assert list(r.feed(b"\x00" * 8)) == []
    assert r.empty == 8
    assert r.bad_crc == 0


if __name__ == "__main__":
    failures = 0
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            try:
                fn()
                print(f"ok   {name}")
            except Exception as exc:  # noqa: BLE001
                failures += 1
                print(f"FAIL {name}: {exc}")
    print(f"\n{failures} failure(s)")
    sys.exit(1 if failures else 0)
