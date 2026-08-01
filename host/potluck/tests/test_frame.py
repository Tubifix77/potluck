"""Golden-vector tests for the Python Potluck Frame decoder.

The byte arrays here are the *same* ones asserted in tests/test_frame_golden.cpp.
That is the entire value of this file: two implementations written from
ARCHITECTURE.md section 5, agreeing on the same bytes, is a test of the
specification. One implementation tested against itself is a test of nothing.

Run with:  python -m pytest host/potluck/tests -q
       or:  python host/potluck/tests/test_frame.py   (no pytest needed)
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from potluck import frame as fr  # noqa: E402
from potluck import payloads as pl  # noqa: E402


# --------------------------------------------------------------------------------------------
# Golden frames -- identical to tests/test_frame_golden.cpp
# --------------------------------------------------------------------------------------------

HEARTBEAT_GOLDEN = bytes(
    [
        0xD9,        # magic
        0x14,        # version 1 | ACKREQ
        0x02, 0x01,  # src 0x0102
        0x04, 0x03,  # dst 0x0304
        0x03,        # HEARTBEAT
        0x65,        # (L3 << 5) | 5
        0x06, 0x05,  # seq 0x0506
        0x08, 0x07,  # msg_id 0x0708
        0x00, 0x00,  # frag_off 0
        0x04, 0x00,  # total_len 4
        0xAA, 0xBB, 0xCC, 0xDD,
    ]
)

AUTH_GOLDEN = bytes(
    [
        0xD9, 0x12,
        0x01, 0x00,
        0x02, 0x00,
        0x01,        # HELLO
        0x60,        # L3, priority 0
        0x00, 0x00,
        0x00, 0x00,
        0x00, 0x00,
        0x02, 0x00,  # total_len 2
        0x11, 0x22,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  # auth_tag, zeroed
    ]
)

FRAGMENT_GOLDEN = bytes(
    [
        0xD9, 0x18,
        0xFF, 0xFF,
        0x00, 0x00,
        0x7F,        # ERR
        0x9F,        # (L4 << 5) | 31
        0xFF, 0xFF,  # seq at the wrap boundary
        0x34, 0x12,  # msg_id 0x1234
        0x10, 0x00,  # frag_off 16
        0x64, 0x00,  # total_len 100
        0x01, 0x02, 0x03, 0x04,
    ]
)

EMPTY_GOLDEN = bytes(
    [0xD9, 0x10, 0x01, 0x00, 0xFF, 0xFF, 0x04, 0x60, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00]
)

HELLO_PAYLOAD_GOLDEN = bytes(
    [
        0x44, 0x33, 0x22, 0x11,  # boot_epoch 0x11223344
        0x00, 0x00, 0x00, 0x00,  # caps
        0x02, 0x01,              # node_id 0x0102
        0x00, 0x00,              # reserved0
        0x02,                    # espnow_version
        0x0A,                    # hb_period_cs = 10 -> 100 ms
        0x06,                    # hb_miss_limit
        0x01,                    # flags: want ack
        0, 0, 0, 0, 0, 0, 0, 0,  # pubkey_fp
    ]
)

HEARTBEAT_PAYLOAD_GOLDEN = bytes(
    [
        0x03, 0x02, 0x01, 0x00,  # uptime_ms      0x00010203
        0x07, 0x06, 0x05, 0x04,  # boot_epoch     0x04050607
        0x0B, 0x0A, 0x09, 0x08,  # hb_seq         0x08090A0B
        0x0F, 0x0E, 0x0D, 0x0C,  # tx_frames      0x0C0D0E0F
        0x13, 0x12, 0x11, 0x10,  # tx_cb_ok       0x10111213
        0x17, 0x16, 0x15, 0x14,  # tx_cb_fail     0x14151617
        0x1B, 0x1A, 0x19, 0x18,  # rx_frames      0x18191A1B
        0x1F, 0x1E, 0x1D, 0x1C,  # rx_lost_seqgap 0x1C1D1E1F
        0x23, 0x22, 0x21, 0x20,  # turnaround_us  0x20212223
        0x25, 0x24,              # ack_of_msg_id  0x2425
        0x27, 0x26,              # rtt_min_us_d8  0x2627
        0x29, 0x28,              # rtt_max_us_d8  0x2829
        0x2B, 0x2A,              # free_dram_kib  0x2A2B
        0x2C,                    # espnow_version
        0x2D,                    # hb_flags
        0x2F, 0x2E,              # reserved0
    ]
)


def test_heartbeat_golden_decodes_to_the_specified_fields():
    f = fr.parse(HEARTBEAT_GOLDEN)
    assert f.magic == fr.MAGIC
    assert f.version == 1
    assert f.src == 0x0102
    assert f.dst == 0x0304
    assert f.opcode == fr.Op.HEARTBEAT
    assert f.lclass == 3
    assert f.priority == 5
    assert f.seq == 0x0506
    assert f.msg_id == 0x0708
    assert f.frag_off == 0
    assert f.total_len == 4
    assert f.payload == bytes([0xAA, 0xBB, 0xCC, 0xDD])
    assert f.wants_ack and not f.has_auth and not f.is_frag
    assert f.auth_tag is None


def test_python_encoder_reproduces_every_golden_frame():
    # If the two implementations agree on these bytes, they agree on section 5.1.
    assert (
        fr.encode(
            src=0x0102, dst=0x0304, opcode=fr.Op.HEARTBEAT, lclass=3, priority=5,
            seq=0x0506, msg_id=0x0708, payload=bytes([0xAA, 0xBB, 0xCC, 0xDD]), ack_req=True,
        )
        == HEARTBEAT_GOLDEN
    )
    assert (
        fr.encode(src=1, dst=2, opcode=fr.Op.HELLO, lclass=3,
                  payload=bytes([0x11, 0x22]), auth=True)
        == AUTH_GOLDEN
    )
    assert (
        fr.encode(src=0xFFFF, dst=0x0000, opcode=fr.Op.ERR, lclass=4, priority=31,
                  seq=0xFFFF, msg_id=0x1234, payload=bytes([1, 2, 3, 4]),
                  frag=True, frag_off=16, total_len=100)
        == FRAGMENT_GOLDEN
    )
    assert (
        fr.encode(src=1, dst=fr.NODE_BROADCAST, opcode=fr.Op.BYE, lclass=3, seq=7)
        == EMPTY_GOLDEN
    )


def test_auth_tag_is_present_and_zero():
    f = fr.parse(AUTH_GOLDEN)
    assert f.has_auth
    # section 14: the bytes are reserved from day one, and M5 is what fills them.
    assert f.auth_tag == bytes(8)
    assert f.payload == bytes([0x11, 0x22])


def test_fragment_fields():
    f = fr.parse(FRAGMENT_GOLDEN)
    assert f.is_frag
    assert f.frag_off == 16
    assert f.total_len == 100
    assert f.seq == 0xFFFF
    assert f.priority == 31
    assert f.lclass == 4


def test_empty_payload_frame_is_sixteen_bytes():
    f = fr.parse(EMPTY_GOLDEN)
    assert len(EMPTY_GOLDEN) == 16
    assert f.payload == b""
    assert f.total_len == 0
    assert f.opcode == fr.Op.BYE


def test_round_trip_over_the_interesting_space():
    for lclass in range(fr.CLASS_MAX + 1):
        for priority in (0, 1, 30, 31):
            for n in (0, 1, 47, 48, 226, 1446):
                for auth in (False, True):
                    payload = bytes((i * 7 + 3) & 0xFF for i in range(n))
                    raw = fr.encode(
                        src=0x1234, dst=0x5678, opcode=fr.Op.ERR, lclass=lclass,
                        priority=priority, seq=n, msg_id=n, payload=payload, auth=auth,
                    )
                    f = fr.parse(raw)
                    assert f.payload == payload
                    assert f.lclass == lclass
                    assert f.priority == priority
                    assert f.has_auth == auth


def _rejects(raw: bytes, reason: str, max_payload: int = fr.MAX_PAYLOAD_V2) -> None:
    try:
        fr.parse(raw, max_payload)
    except fr.FrameError as exc:
        assert exc.reason == reason, f"expected {reason}, got {exc.reason}"
        return
    raise AssertionError(f"expected {reason}, but the frame was accepted")


def test_rejections_match_the_firmwares_reasons():
    base = bytearray(HEARTBEAT_GOLDEN)

    for b in range(256):
        if b == fr.MAGIC:
            continue
        m = bytearray(base)
        m[0] = b
        _rejects(bytes(m), "bad_magic")

    for v in range(16):
        if v == fr.VERSION:
            continue
        m = bytearray(base)
        m[1] = (v << 4)
        _rejects(bytes(m), "bad_version")

    m = bytearray(base)
    m[1] = (fr.VERSION << 4) | fr.FLAG_RESERVED
    _rejects(bytes(m), "reserved_flag_set")

    for cls in range(5, 8):
        m = bytearray(base)
        m[7] = cls << 5
        _rejects(bytes(m), "bad_class")

    for n in range(fr.HEADER_SIZE):
        _rejects(bytes(base[:n]), "too_short")

    # Unfragmented with a non-zero frag_off, and with a total_len that disagrees.
    m = bytearray(base)
    m[12] = 4
    _rejects(bytes(m), "length_mismatch")
    for total in (3, 5):
        m = bytearray(base)
        m[14] = total
        _rejects(bytes(m), "length_mismatch")

    # section 5.4: HELLO, HEARTBEAT and SAFE_STATE must not fragment.
    for op in (fr.Op.HELLO, fr.Op.HEARTBEAT, fr.Op.SAFE_STATE):
        m = bytearray(base)
        m[1] = (fr.VERSION << 4) | fr.FLAG_FRAG
        m[6] = int(op)
        m[12] = 0
        m[14] = 64
        _rejects(bytes(m), "frag_not_allowed")

    # AUTH set with no room for the tag.
    m = bytearray(EMPTY_GOLDEN)
    m[1] = (fr.VERSION << 4) | fr.FLAG_AUTH
    _rejects(bytes(m), "missing_auth_tag")


def test_a_v2_frame_truncated_to_250_bytes_is_rejected():
    # The section 5.3 hazard: a v1.0 receiver handed a longer v2.0 frame may deliver
    # only its first 250 bytes. Parsing that as if whole is worse than dropping it.
    raw = fr.encode(src=1, dst=2, opcode=fr.Op.ERR, payload=b"\x5a" * 1000)
    _rejects(raw[:250], "length_mismatch")


def test_the_v1_profile_cap_is_enforced_on_receive():
    raw = fr.encode(src=1, dst=2, opcode=fr.Op.ERR, payload=b"\x77" * 300)
    fr.parse(raw, fr.MAX_PAYLOAD_V2)  # fine on a v2 link
    _rejects(raw, "payload_too_long", fr.MAX_PAYLOAD_V1)

    at_cap = fr.encode(src=1, dst=2, opcode=fr.Op.ERR, payload=b"\x33" * fr.MAX_PAYLOAD_V1)
    fr.parse(at_cap, fr.MAX_PAYLOAD_V1)  # exactly at the cap is legal


def test_profile_arithmetic_matches_section_5_3():
    assert fr.MAX_PAYLOAD_V2 == 1446
    assert fr.MAX_PAYLOAD_V1 == 226
    assert fr.ESPNOW_V2_LINK_MTU - fr.HEADER_SIZE - fr.AUTH_TAG_SIZE == 1446
    assert fr.ESPNOW_V1_LINK_MTU - fr.HEADER_SIZE - fr.AUTH_TAG_SIZE == 226


# --------------------------------------------------------------------------------------------
# Payload layouts
# --------------------------------------------------------------------------------------------


def test_hello_payload_golden():
    h = pl.Hello.parse(HELLO_PAYLOAD_GOLDEN)
    assert h.boot_epoch == 0x11223344
    assert h.caps == 0
    assert h.node_id == 0x0102
    assert h.espnow_version == 2
    assert h.hb_period_ms == 100        # section 8.2
    assert h.hb_miss_limit == 6         # section 8.2
    assert h.dead_after_ms == 600       # section 8.2
    assert h.wants_ack
    assert h.pubkey_fp == bytes(8)      # zero until M5


def test_heartbeat_payload_golden():
    hb = pl.Heartbeat.parse(HEARTBEAT_PAYLOAD_GOLDEN)
    assert hb.uptime_ms == 0x00010203
    assert hb.boot_epoch == 0x04050607
    assert hb.hb_seq == 0x08090A0B
    assert hb.tx_frames == 0x0C0D0E0F
    assert hb.tx_cb_ok == 0x10111213
    assert hb.tx_cb_fail == 0x14151617
    assert hb.rx_frames == 0x18191A1B
    assert hb.rx_lost_seqgap == 0x1C1D1E1F
    assert hb.turnaround_us == 0x20212223
    assert hb.ack_of_msg_id == 0x2425
    assert hb.rtt_min_us == 0x2627 * 8
    assert hb.rtt_max_us == 0x2829 * 8
    assert hb.free_dram_kib == 0x2A2B
    assert hb.espnow_version == 0x2C
    assert hb.hb_flags == 0x2D
    assert hb.is_reply  # 0x2D has bit 0 set


def test_unmeasured_rtt_is_none_not_zero():
    raw = bytearray(HEARTBEAT_PAYLOAD_GOLDEN)
    raw[38:40] = (pl.RTT_UNKNOWN_D8).to_bytes(2, "little")
    raw[40:42] = (pl.RTT_UNKNOWN_D8).to_bytes(2, "little")
    hb = pl.Heartbeat.parse(bytes(raw))
    assert hb.rtt_min_us is None
    assert hb.rtt_max_us is None


def test_pdr_is_none_when_nothing_was_attempted():
    raw = bytearray(48)
    hb = pl.Heartbeat.parse(bytes(raw))
    assert hb.tx_pdr is None
    assert hb.rx_pdr is None


def test_pdr_arithmetic():
    raw = bytearray(HEARTBEAT_PAYLOAD_GOLDEN)
    raw[16:20] = (990).to_bytes(4, "little")   # tx_cb_ok
    raw[20:24] = (10).to_bytes(4, "little")    # tx_cb_fail
    raw[24:28] = (950).to_bytes(4, "little")   # rx_frames
    raw[28:32] = (50).to_bytes(4, "little")    # rx_lost_seqgap
    hb = pl.Heartbeat.parse(bytes(raw))
    assert abs(hb.tx_pdr - 0.99) < 1e-9
    assert abs(hb.rx_pdr - 0.95) < 1e-9


def test_a_short_payload_is_refused_not_padded():
    for n in range(48):
        try:
            pl.Heartbeat.parse(bytes(n))
        except pl.ShortPayload:
            continue
        raise AssertionError(f"{n} bytes should not parse as a 48-byte HEARTBEAT")
    pl.Heartbeat.parse(bytes(48))

    # Extra trailing bytes are ignored, so a decoder written today survives a
    # later milestone appending a field.
    pl.Heartbeat.parse(bytes(48) + b"future field")


def test_payload_sizes_match_the_firmware_asserts():
    assert pl._HELLO.size == 24
    assert pl._HELLO_ACK.size == 12
    assert pl._HEARTBEAT.size == 48
    assert pl._BYE.size == 8
    assert pl._ERR.size == 8
    # section 5.4 forbids fragmenting HELLO and HEARTBEAT, so both must fit the
    # 226-byte v1 floor unfragmented.
    assert pl._HELLO.size <= fr.MAX_PAYLOAD_V1
    assert pl._HEARTBEAT.size <= fr.MAX_PAYLOAD_V1


def test_bye_and_err_payloads():
    b = pl.Bye.parse(bytes([1, 0, 0, 0, 0x2A, 0x00, 0x00, 0x00]))
    assert b.boot_epoch == 1 and b.node_id == 42 and b.reason == 0

    detail = b"bad_magic"
    raw = (
        (0x0002).to_bytes(2, "little")
        + (0x1234).to_bytes(2, "little")
        + bytes([len(detail)])
        + bytes(3)
        + detail
    )
    e = pl.Err.parse(raw)
    assert e.code == 2 and e.ref_msg_id == 0x1234 and e.detail == "bad_magic"


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
