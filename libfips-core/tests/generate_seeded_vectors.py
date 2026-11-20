# Independent Python translation of pinned core/src/testutil.rs SplitMix64.
# Run once to emit C literals; no C fake is imported or executed.
seeds = (0, 0x0123456789abcdef, 0xffffffffffffffff)
mask = (1 << 64) - 1
print('static const char *const expected_hex[3] = {')
for seed in seeds:
    state, data = seed, b""
    for _ in range(128):
        state = (state + 0x9e3779b97f4a7c15) & mask
        z = ((state ^ (state >> 30)) * 0xbf58476d1ce4e5b9) & mask
        z = ((z ^ (z >> 27)) * 0x94d049bb133111eb) & mask
        data += (z ^ (z >> 31)).to_bytes(8, "little")
    print('    /* seed 0x%016x */' % seed)
    print('\n'.join('    "' + data.hex()[i:i+64] + '"' for i in range(0, 2048, 64)) + ',')
print('};')
