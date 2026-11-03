"""
check_raw.py - check the recorder's files (BATnnnnn.WAV) on the PC, and
files of raw ADC codes (.RAW, as the archived SD write test wrote).

A .RAW file is raw little-endian uint16 ADC codes copied from the PSRAM ring
in 512-byte blocks. A 12-bit ADC code is at most 4095, so any larger 16-bit
word means a corrupted, shifted or padded block. CRC checking is off in SPI
mode, so this is the check that the card stored what the board sent (diary 40).

A .WAV file (recorder.c) is a 512-byte header, then the same blocks as
signed 16-bit samples, (code - 2048) x 16. Their low 4 bits are always 0, so
a sample with any of them set means a corrupted or shifted block. The header
is checked too, and the file is opened with Python's wave module, as any
WAV reader would.

    python -I Tools/check_raw.py E:\\SB08P.RAW E:\\BAT00001.WAV

Needs: python -m pip install numpy
"""

import struct
import sys
import wave
from pathlib import Path

import numpy as np


BLOCK_SAMPLES = 256         # AUDIO_BLOCK_SIZE (pipeline_config.h)
SAMPLE_RATE_HZ = 250_000    # AUDIO_SAMPLE_RATE_HZ
ADC_MAX = 4095              # 12-bit ADC
ADC_MIDSCALE = 2048

# recorder.c: the audio starts one sector in, so it stays sector-aligned
WAV_DATA_OFFSET = 512
WAV_SAMPLE_SHIFT = 4        # 12-bit codes x 16


def check(path):
    with open(path, "rb") as file:
        is_wav = file.read(4) == b"RIFF"

    return check_wav(path) if is_wav else check_raw(path)


def check_raw(path):
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

    check_blocks(data)
    print("  PASS" if passed else "  FAIL")
    return passed


def read_chunks(raw):
    """RIFF chunks after the 'WAVE' id: {id: (offset of its data, its size)}"""
    chunks = {}
    offset = 12
    while offset + 8 <= len(raw):
        chunk_id, size = struct.unpack_from("<4sI", raw, offset)
        chunks.setdefault(chunk_id, (offset + 8, size))
        offset += 8 + size + (size & 1)     # chunks are padded to an even length
        if chunk_id == b"data":
            break                           # the audio runs to the end of the file
    return chunks


def check_wav(path):
    raw = path.read_bytes()
    passed = True

    print(f"{path}: {len(raw):,} bytes")

    riff_bytes, wave_id = struct.unpack_from("<I4s", raw, 4)
    chunks = read_chunks(raw)
    if wave_id != b"WAVE" or b"fmt " not in chunks or b"data" not in chunks:
        print(f"  FAIL: not a WAV file (chunks: {', '.join(c.decode(errors='replace') for c in chunks)})")
        return False

    fmt_offset, _ = chunks[b"fmt "]
    audio_format, channels, rate, byte_rate, align, bits = struct.unpack_from("<HHIIHH", raw, fmt_offset)
    expected = (1, 1, SAMPLE_RATE_HZ, SAMPLE_RATE_HZ * 2, 2, 16)
    if (audio_format, channels, rate, byte_rate, align, bits) != expected:
        print(f"  FAIL: format {audio_format}, {channels} channel(s), {rate} Hz, {bits}-bit "
              f"(expected PCM, mono, {SAMPLE_RATE_HZ} Hz, 16-bit)")
        passed = False
    else:
        print(f"  PCM, mono, {rate} Hz, 16-bit")

    data_offset, data_bytes = chunks[b"data"]
    after = len(raw) - data_offset - data_bytes
    if data_offset != WAV_DATA_OFFSET:
        print(f"  FAIL: the audio starts at byte {data_offset}, not {WAV_DATA_OFFSET}: not sector-aligned")
        passed = False
    if riff_bytes != data_offset + data_bytes - 8:
        print(f"  FAIL: the RIFF size ({riff_bytes:,}) doesn't match the data chunk's end "
              f"({data_offset + data_bytes - 8:,})")
        passed = False
    if after < 0:
        print(f"  FAIL: the header claims {-after:,} bytes more audio than the file holds")
        passed = False
    elif after > 0:
        # the header is brought up to date every 2 s: more after it means the
        # recording didn't close, e.g. a power cut
        print(f"  WARNING: {after:,} bytes after the data chunk ({after / (2 * SAMPLE_RATE_HZ):.2f} s): "
              f"saved after the last sync, the file wasn't closed")
    if data_bytes % (BLOCK_SAMPLES * 2):
        print("  FAIL: the audio isn't a whole number of 512-byte blocks")
        passed = False

    samples = np.frombuffer(raw, dtype="<i2", count=data_bytes // 2, offset=data_offset) \
        if after >= 0 else np.frombuffer(raw[data_offset:], dtype="<i2")
    blocks = samples.size // BLOCK_SAMPLES
    print(f"  {data_bytes:,} bytes of audio, {blocks:,} blocks, {samples.size / SAMPLE_RATE_HZ:.2f} s")

    low_bits = (1 << WAV_SAMPLE_SHIFT) - 1
    bad = np.flatnonzero(samples & low_bits)
    if bad.size:
        bad_blocks = np.unique(bad // BLOCK_SAMPLES)
        print(f"  FAIL: {bad.size:,} samples with low bits set in {bad_blocks.size} block(s); first in block "
              f"{bad_blocks[0]}, sample {bad[0] % BLOCK_SAMPLES}: 0x{int(samples[bad[0]]) & 0xFFFF:04X}")
        print("        blocks " + ", ".join(str(b) for b in bad_blocks[:12]) + (" ..." if bad_blocks.size > 12 else ""))
        passed = False
    else:
        print(f"  every sample is a 12-bit ADC code x {1 << WAV_SAMPLE_SHIFT}")

    # any reader should agree on the length and rate
    try:
        with wave.open(str(path), "rb") as reader:
            frames, framerate = reader.getnframes(), reader.getframerate()
        if frames != data_bytes // 2 or framerate != SAMPLE_RATE_HZ:
            print(f"  FAIL: Python's wave module reads {frames:,} samples at {framerate} Hz")
            passed = False
        else:
            print(f"  Python's wave module agrees: {frames:,} samples at {framerate} Hz")
    except (wave.Error, EOFError) as error:
        print(f"  FAIL: Python's wave module can't read it: {error}")
        passed = False

    # back to ADC codes for the block checks (a whole 0 block = a failed ring read, saved as silence)
    check_blocks((samples.astype(np.int32) >> WAV_SAMPLE_SHIFT) + ADC_MIDSCALE)
    print("  PASS" if passed else "  FAIL")
    return passed


def check_blocks(data):
    """Checks common to both kinds of file, on the ADC codes"""
    blocks = data.size // BLOCK_SAMPLES
    if blocks == 0:
        return

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


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)

    results = [check(Path(name)) for name in sys.argv[1:]]
    sys.exit(0 if all(results) else 1)


if __name__ == "__main__":
    main()
