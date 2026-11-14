#!/usr/bin/env python3
"""python3 -I <absolute path>: regenerate bloom_pins.h (stdout only).

Binary64 operations deliberately follow core/src/proto/bloom/core.rs.
Python math.log calls the host C log. math_log_count is an independent
approximate check, NOT the protocol bit oracle. count uses the literal
Rust libm 0.2.16 outputs below (regenerate with bloom_log_oracle.rs). SHA indices wrap in u64 before size_t cast.
"""
import hashlib
import math
import struct


def bits(x):
    return struct.unpack('<Q', struct.pack('<d', x))[0]


def powi(base, exp):
    result = 1.0
    while exp:
        if exp & 1:
            result *= base
        base *= base
        exp >>= 1
    return result


# Exact Rust libm bits exported by the offline reference in bloom_log_oracle.rs.
# All other outputs below (including the independent estimate) use math.log.
LIBM_COUNT = {
    (64, 3, 0): 0x8000000000000000,
    (64, 3, 1): 0x3fd5807320c475b4,
    (64, 3, 12): 0x4011b7f37db1e698,
    (64, 3, 14): 0x401510bd9dcd2251,
    (64, 3, 32): 0x402d9303fea2f7e9,
    (64, 3, 63): 0x40562e42fefa39ee,
    (1024, 5, 0): 0x8000000000000000,
    (1024, 5, 1): 0x3fc99ccd556ef40f,
    (1024, 5, 12): 0x4003503a1c6d4eef,
    (1024, 5, 14): 0x40068df603d2b26c,
    (1024, 5, 84): 0x4031877b71460e3d,
    (1024, 5, 124): 0x403a6f5abc305cd7,
    (1024, 5, 129): 0x403b93700938f46a,
    (1024, 5, 145): 0x403f4530fcd0ba5a,
    (1024, 5, 250): 0x404ca968a5a75cba,
    (1024, 5, 512): 0x4061be9bff2e94bf,
    (1024, 5, 1023): 0x40962e42fefa39f0,
    (1024, 7, 0): 0x8000000000000000,
    (1024, 7, 1): 0x3fc24b6e1873d2e5,
    (1024, 7, 12): 0x3ffb972e71c0b9e6,
    (1024, 7, 14): 0x40001c4202bb11ba,
    (1024, 7, 84): 0x40290ab058ad38e9,
    (1024, 7, 124): 0x4032e1d318b4d499,
    (1024, 7, 129): 0x4033b27498df8a02,
    (1024, 7, 145): 0x403655fe6b70851b,
    (1024, 7, 250): 0x404479019ae5423b,
    (1024, 7, 512): 0x4059597123671da3,
    (1024, 7, 1023): 0x408fafcd6c40e50c,
    (2048, 7, 0): 0x8000000000000000,
    (2048, 7, 1): 0x3fc24a493cf6189c,
    (2048, 7, 12): 0x3ffb825dce1be860,
    (2048, 7, 14): 0x40000e106ae3848f,
    (2048, 7, 84): 0x4028818e0dbb35a3,
    (2048, 7, 124): 0x403245f2e5869d25,
    (2048, 7, 129): 0x403308d822da408f,
    (2048, 7, 145): 0x40357bf097fe238d,
    (2048, 7, 250): 0x40430b75382bf461,
    (2048, 7, 1024): 0x4069597123671da3,
    (2048, 7, 2047): 0x40a16d7dc856e460,
    (4096, 5, 0): 0x8000000000000000,
    (4096, 5, 1): 0x3fc99a666eef555c,
    (4096, 5, 12): 0x40033a6a02079e34,
    (4096, 5, 14): 0x40067038ee6db344,
    (4096, 5, 84): 0x4030f98329e5c451,
    (4096, 5, 124): 0x40392ee278602045,
    (4096, 5, 129): 0x403a370afadfbf64,
    (4096, 5, 145): 0x403d86979b4307c0,
    (4096, 5, 250): 0x4049cba46dd43a08,
    (4096, 5, 2048): 0x4081be9bff2e94bf,
    (4096, 5, 4095): 0x40ba9de9fec5df1f,
    (8192, 1, 0): 0x8000000000000000,
    (8192, 1, 1): 0x3ff0004001555d56,
    (8192, 1, 12): 0x4028048120511854,
    (8192, 1, 14): 0x402c0621c9eb99ee,
    (8192, 1, 84): 0x40551bc09bbf435c,
    (8192, 1, 124): 0x405f3cacf1cd3b27,
    (8192, 1, 129): 0x406040d8e4cf289e,
    (8192, 1, 145): 0x4062498e38244552,
    (8192, 1, 250): 0x406fbc9cb3081482,
    (8192, 1, 4096): 0x40b62e42fefa39ef,
    (8192, 1, 4480): 0x40b954aecaa56320,
    (8192, 1, 4512): 0x40b99b9c10afe553,
    (8192, 1, 5936): 0x40c4a20ea197203d,
    (8192, 1, 5944): 0x40c4b09b982288c2,
    (8192, 1, 8191): 0x40f205966f2b4f12,
    (8192, 4, 0): 0x8000000000000000,
    (8192, 4, 1): 0x3fd0004001555d56,
    (8192, 4, 12): 0x4008048120511854,
    (8192, 4, 14): 0x400c0621c9eb99ee,
    (8192, 4, 84): 0x40351bc09bbf435c,
    (8192, 4, 124): 0x403f3cacf1cd3b27,
    (8192, 4, 129): 0x404040d8e4cf289e,
    (8192, 4, 145): 0x4042498e38244552,
    (8192, 4, 250): 0x404fbc9cb3081482,
    (8192, 4, 4096): 0x40962e42fefa39ef,
    (8192, 4, 4480): 0x409954aecaa56320,
    (8192, 4, 4512): 0x40999b9c10afe553,
    (8192, 4, 5936): 0x40a4a20ea197203d,
    (8192, 4, 5944): 0x40a4b09b982288c2,
    (8192, 4, 8191): 0x40d205966f2b4f12,
    (8192, 5, 0): 0x8000000000000000,
    (8192, 5, 1): 0x3fc99a0002222ef0,
    (8192, 5, 12): 0x400336cdb3741377,
    (8192, 5, 14): 0x40066b4e3b22e18c,
    (8192, 5, 84): 0x4030e3007c9902b0,
    (8192, 5, 124): 0x4038fd5727d76286,
    (8192, 5, 129): 0x403a015b07b1da97,
    (8192, 5, 145): 0x403d427d26a06eea,
    (8192, 5, 250): 0x404963b08f39aa02,
    (8192, 5, 4096): 0x4091be9bff2e94bf,
    (8192, 5, 4480): 0x409443bf08844f4d,
    (8192, 5, 4512): 0x40947c7cda265110,
    (8192, 5, 5936): 0x40a081a54e128031,
    (8192, 5, 5944): 0x40a08d4946820702,
    (8192, 5, 8191): 0x40ccd5bd7eabb1b7,
    (8192, 6, 0): 0x8000000000000000,
    (8192, 6, 1): 0x3fc555aaac71d1c8,
    (8192, 6, 12): 0x40000300c0361038,
    (8192, 6, 14): 0x4002aec13147bbf4,
    (8192, 6, 84): 0x402c2500cfa9af25,
    (8192, 6, 124): 0x4034d31df688d21a,
    (8192, 6, 129): 0x4035abcbdbbee0d2,
    (8192, 6, 145): 0x40386212f585b1c2,
    (8192, 6, 250): 0x40452868775ab856,
    (8192, 6, 4096): 0x408d9303fea2f7e9,
    (8192, 6, 4480): 0x4090e31f31c3976a,
    (8192, 6, 4512): 0x409112680b1fee37,
    (8192, 6, 5936): 0x409b82be2cc98051,
    (8192, 6, 5944): 0x409b9624cad8b658,
    (8192, 6, 8191): 0x40c807733ee4696d,
    (8192, 7, 0): 0x8000000000000000,
    (8192, 7, 1): 0x3fc2496db8618f3d,
    (8192, 7, 12): 0x3ffb72dcb7381bcd,
    (8192, 7, 14): 0x4000038105ab3363,
    (8192, 7, 84): 0x40281fb78d6cdf44,
    (8192, 7, 124): 0x4031d9876599d8a8,
    (8192, 7, 129): 0x40329341057f09d9,
    (8192, 7, 145): 0x4034e65964bbbcf0,
    (8192, 7, 250): 0x404222a2af7254dc,
    (8192, 7, 4096): 0x4089597123671da3,
    (8192, 7, 4480): 0x408cf310e7987149,
    (8192, 7, 4512): 0x408d442013122aa8,
    (8192, 7, 5936): 0x409794a301d1496a,
    (8192, 7, 5944): 0x4097a5441b952e94,
    (8192, 7, 8191): 0x40c498abecc3c814,
    (8192, 255, 0): 0x8000000000000000,
    (8192, 255, 1): 0x3f70105051a7045a,
    (8192, 255, 12): 0x3fa81c9dbe0f277b,
    (8192, 255, 14): 0x3fac22440df99381,
    (8192, 255, 84): 0x3fd530f18d4c8fec,
    (8192, 255, 124): 0x3fdf5c08fac8032a,
    (8192, 255, 129): 0x3fe0512a0ede06a5,
    (8192, 255, 145): 0x3fe25bea22468bde,
    (8192, 255, 250): 0x3fefdc792c3448cb,
    (8192, 255, 4096): 0x403644878680baaa,
    (8192, 255, 4480): 0x40396e1ce78cf010,
    (8192, 255, 4512): 0x4039b5516211f74a,
    (8192, 255, 5936): 0x4044b6c566fe1e5b,
    (8192, 255, 5944): 0x4044c560f91ba466,
    (8192, 255, 8191): 0x407217ae1d4897aa,
    (16384, 7, 0): 0x8000000000000000,
    (16384, 7, 1): 0x3fc2494924f3d061,
    (16384, 7, 12): 0x3ffb704976e701bc,
    (16384, 7, 14): 0x400001c041600f35,
    (16384, 7, 84): 0x40280fcdd59f2e2a,
    (16384, 7, 124): 0x4031c81add2b0357,
    (16384, 7, 129): 0x4032806288a2f854,
    (16384, 7, 145): 0x4034ce76403a84a9,
    (16384, 7, 250): 0x4041feaa2a035e9c,
    (16384, 7, 8192): 0x4099597123671da3,
    (16384, 7, 16383): 0x40d62e42fefa39ef,
}
print('/* Generated by python3 -I tests/tools/bloom_vectors.py; do not edit. */')
print('static const struct { size_t m; uint8_t k; size_t x; uint64_t fpr, count, math_log_count; } estimate_pins[] = {')
# Includes every custom (m,k) used by upstream bloom core/wire tests and
# populations at the rejection boundaries, empty, half, and one below full.
for m, k in [(64, 3), (1024, 5), (1024, 7), (2048, 7), (4096, 5),
             (8192, 1), (8192, 4), (8192, 5), (8192, 6), (8192, 7),
             (8192, 255), (16384, 7)]:
    for x in sorted({0, 1, min(250, m - 1), m // 2, m - 1} |
                    {x for x in (12, 14, 84, 124, 129, 145) if x < m} |
                    ({4480, 4512, 5936, 5944} if m == 8192 else set())):
        fill = float(x) / float(m)
        count = -(float(m) / float(k)) * math.log(1.0 - fill)
        print(f'    {{{m}, {k}, {x}, UINT64_C(0x{bits(powi(fill, k)):016x}), UINT64_C(0x{LIBM_COUNT[m,k,x]:016x}), UINT64_C(0x{bits(count):016x})}},')
print('};')
print('static const struct { uint8_t addr[16]; size_t indices[5]; } index_pins[] = {')
for addr in [bytes(16), bytes([7]) + bytes(15), bytes([255]) * 16, bytes(range(16))]:
    h1, h2 = struct.unpack('<QQ', hashlib.sha256(addr).digest()[:16])
    indices = [((h1 + i * h2) & ((1 << 64) - 1)) % 8192 for i in range(5)]
    print('    {{' + ','.join(str(x) for x in addr) + '}, {' + ','.join(str(x) for x in indices) + '}},')
print('};')
