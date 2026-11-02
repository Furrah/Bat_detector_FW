"""
check_raw.py - check the SD test's files (SBnnX.RAW, from capture.py sdtest) on the PC.

Each file is raw little-endian uint16 ADC codes copied from the PSRAM ring in
512-byte blocks. A 12-bit ADC code is at most 4095, so any larger 16-bit word
means a corrupted, shifted or padded block. CRC checking is off in SPI mode,
so this is the check that the card stored what the board sent (diary 40).

    python -I Tools/check_raw.py E:\\SB08P.RAW E:\\SB01D.RAW

Needs: python -m pip install numpy
"""

import sys
from pathlib import Path

import numpy as np


BLOCK_SAMPLES = 256         # AUDIO_BLOCK_SIZE (pipeline_config.h)
SAMPLE_RATE_HZ = 250_000    # AUDIO_SAMPLE_RATE_HZ
ADC_MAX = 4095              # 12-bit ADC


def check(path):
    size = path.stat().st_size
    data = np.fromfile(path, dtype="<u2")
    blocks = data.size // BLOCK_SAMPLES
    passed = True

    print(f"{path}: {size:,} bytes, {blocks:,} blocks, {data.size / SAMPLE_RATE_HZ:.1f} s of audio")

    if size % (BLOCK_SAMPLES * 2):
        print("  FAIL: not a whole number of 512-byte blocks")
        passed = False

    bad = np.flatnonzero(data > ADC_MAX)
    if bad.size:
        bad_blocks = np.unique(bad // BLOCK_SAMPLES)
        print(f"  FAIL: {bad.size:,} words above {ADC_MAX} in {bad_blocks.size} block(s); first in block "
              f"{bad_blocks[0]}, word {bad[0] % BLOCK_SAMPLES}: 0x{int(data[bad[0]]):04X}")
        print("        blocks " + ", ".join(str(b) for b in bad_blocks[:12]) + (" ..." if bad_blocks.size > 12 else ""))
        passed = False
    else:
        print(f"  every word is a 12-bit ADC code (0-{ADC_MAX})")

    whole = data[:blocks * BLOCK_SAMPLES].reshape(blocks, BLOCK_SAMPLES)

    # a whole block of one value (e.g. 0x0000 or 0xFFFF) is what a lost write leaves
    flat = np.flatnonzero((whole == whole[:, :1]).all(axis=1))
    if flat.size:
        print(f"  WARNING: {flat.size} block(s) are one repeated value; first is block {flat[0]} "
              f"(0x{int(whole[flat[0], 0]):04X})")

    # steps across block boundaries should look like steps inside blocks
    if blocks > 1:
        signed = whole.astype(np.int32)
        inside = np.abs(np.diff(signed, axis=1)).max()
        across = np.abs(signed[1:, 0] - signed[:-1, -1]).max()
        print(f"  largest sample-to-sample step: {inside} inside blocks, {across} across block boundaries")

    print(f"  codes {int(data.min())}-{int(data.max())}, mean {data.mean():.1f}, rms {data.std():.1f}")
    print("  PASS" if passed else "  FAIL")
    return passed


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)

    results = [check(Path(name)) for name in sys.argv[1:]]
    sys.exit(0 if all(results) else 1)


if __name__ == "__main__":
    main()
