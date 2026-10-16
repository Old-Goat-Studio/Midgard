# SPDX-License-Identifier: Apache-2.0
"""FNV-1a, 64 bits (engine/core/include/midgard/hash.h)."""

OFFSET_BASIS = 0xCBF29CE484222325
PRIME = 0x100000001B3
MASK = (1 << 64) - 1


def fnv1a64(data: bytes) -> int:
    value = OFFSET_BASIS
    for byte in data:
        value = ((value ^ byte) * PRIME) & MASK
    return value
