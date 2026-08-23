"""Ed25519, from RFC 8032's reference algorithm. Host tooling only.

READ THIS BEFORE USING IT FOR ANYTHING

This exists so `potluck sign` has no dependencies, in a project whose test harness is a single
header specifically to avoid the first one. It is a *reference* implementation of the algorithm in
[RFC 8032](https://www.rfc-editor.org/rfc/rfc8032.txt) section 5.1, and it is correct against the
RFC's own published test vectors (tests/test_signing.py checks all three of section 7.1).

What it is not:

  * **Not constant-time.** Every operation is Python big integers, so timing and cache behaviour leak.
    Signing a manifest happens on a developer's machine, on a key that developer already holds, with
    no attacker measuring it -- which is the only reason that is acceptable here.
  * **Not the firmware's verifier.** The node verifies manifests with a vetted library (M5, section
    9.3), and section 13-M5 has an open **[MEASURE]** on the algorithm choice: "Ed25519 vs P-256
    decided on measured verify cost". So the *format* this file feeds carries an `alg` field rather
    than assuming an answer, and nothing here decides that question.
  * **Not a key manager.** It reads and writes key bytes; where those live and who can read them is
    outside its business, and signing.py says the same thing louder.

If a vetted library is ever added as a dependency, this file should become a fallback rather than
stay the default -- and the test vectors are the thing that lets both be trusted equally.
"""

from __future__ import annotations

import hashlib

# --- the curve, from RFC 8032 section 5.1 -----------------------------------------------------

#: 2^255 - 19, the field prime.
P = 2 ** 255 - 19
#: The group order, l in the RFC.
L = 2 ** 252 + 27742317777372353535851937790883648493

_D = -121665 * pow(121666, P - 2, P) % P
_SQRT_M1 = pow(2, (P - 1) // 4, P)

KEY_BYTES = 32
SIGNATURE_BYTES = 64


def _sha512(data: bytes) -> bytes:
    return hashlib.sha512(data).digest()


def _sha512_int(data: bytes) -> int:
    return int.from_bytes(_sha512(data), "little")


def _inv(x: int) -> int:
    return pow(x, P - 2, P)


def _recover_x(y: int, sign: int) -> int | None:
    """The x for this y on the curve, or None if there is none."""
    if y >= P:
        return None
    x2 = (y * y - 1) * _inv(_D * y * y + 1) % P
    if x2 == 0:
        return None if sign else 0
    x = pow(x2, (P + 3) // 8, P)
    if x * x % P != x2:
        x = x * _SQRT_M1 % P
    if x * x % P != x2:
        return None
    if x % 2 != sign:
        x = P - x
    return x


# Points are (X, Y, Z, T) in extended coordinates, which keeps the arithmetic to multiplications.
_Point = tuple[int, int, int, int]

_G_Y = 4 * _inv(5) % P
_G_X = _recover_x(_G_Y, 0)
assert _G_X is not None
G: _Point = (_G_X, _G_Y, 1, _G_X * _G_Y % P)
IDENTITY: _Point = (0, 1, 1, 0)


def _add(p: _Point, q: _Point) -> _Point:
    a = (p[1] - p[0]) * (q[1] - q[0]) % P
    b = (p[1] + p[0]) * (q[1] + q[0]) % P
    c = 2 * p[3] * q[3] * _D % P
    dd = 2 * p[2] * q[2] % P
    e, f, g, h = b - a, dd - c, dd + c, b + a
    return (e * f % P, g * h % P, f * g % P, e * h % P)


def _mul(s: int, p: _Point) -> _Point:
    out = IDENTITY
    while s > 0:
        if s & 1:
            out = _add(out, p)
        p = _add(p, p)
        s >>= 1
    return out


def _equal(p: _Point, q: _Point) -> bool:
    # Projective coordinates compare by cross-multiplication, not component-wise.
    if (p[0] * q[2] - q[0] * p[2]) % P != 0:
        return False
    return (p[1] * q[2] - q[1] * p[2]) % P == 0


def _compress(p: _Point) -> bytes:
    zi = _inv(p[2])
    x = p[0] * zi % P
    y = p[1] * zi % P
    return int.to_bytes(y | ((x & 1) << 255), 32, "little")


def _decompress(data: bytes) -> _Point | None:
    if len(data) != 32:
        return None
    v = int.from_bytes(data, "little")
    sign = v >> 255
    y = v & ((1 << 255) - 1)
    x = _recover_x(y, sign)
    if x is None:
        return None
    return (x, y, 1, x * y % P)


def _secret_expand(secret: bytes) -> tuple[int, bytes]:
    if len(secret) != KEY_BYTES:
        raise ValueError(f"an Ed25519 secret key is {KEY_BYTES} bytes, not {len(secret)}")
    h = _sha512(secret)
    a = int.from_bytes(h[:32], "little")
    a &= (1 << 254) - 8   # clear the low three bits and the top bit...
    a |= 1 << 254         # ...and set bit 254. RFC 8032 section 5.1.5.
    return a, h[32:]


def public_key(secret: bytes) -> bytes:
    """The 32-byte public key for a 32-byte secret."""
    a, _ = _secret_expand(secret)
    return _compress(_mul(a, G))


def sign(secret: bytes, message: bytes) -> bytes:
    """A 64-byte signature over `message`."""
    a, prefix = _secret_expand(secret)
    pub = _compress(_mul(a, G))
    r = _sha512_int(prefix + message) % L
    big_r = _mul(r, G)
    rs = _compress(big_r)
    k = _sha512_int(rs + pub + message) % L
    s = (r + k * a) % L
    return rs + int.to_bytes(s, 32, "little")


def verify(pub: bytes, message: bytes, signature: bytes) -> bool:
    """True if `signature` is a valid Ed25519 signature by `pub` over `message`.

    Returns False rather than raising for every malformed input, because a caller checking a
    signature wants one answer and there is exactly one safe default.
    """
    if len(pub) != KEY_BYTES or len(signature) != SIGNATURE_BYTES:
        return False
    point = _decompress(pub)
    if point is None:
        return False
    big_r = _decompress(signature[:32])
    if big_r is None:
        return False
    s = int.from_bytes(signature[32:], "little")
    if s >= L:
        # A scalar at or above the group order is a malleable encoding of the same signature.
        # Refusing it is what makes "the signature bytes" a stable identity.
        return False
    k = _sha512_int(signature[:32] + pub + message) % L
    return _equal(_mul(s, G), _add(big_r, _mul(k, point)))
