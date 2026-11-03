"""
capture.py - record debug data from the bat detector over its USB serial port.

The board side is Core/Src/debug_link.c; its frame format is described at the
top of that file, and the structs below must match it.

    python -I Tools/capture.py info
    python -I Tools/capture.py raw --count 20 --label quiet-room
    python -I Tools/capture.py energy --seconds 60 --label quiet-room
    python -I Tools/capture.py verify --count 50
    python -I Tools/capture.py stats
    python -I Tools/capture.py stats --window 30
    python -I Tools/capture.py sdtest --seconds 60 --pretrigger 5 --sectors 1 4 8 16
    python -I Tools/capture.py fftcheck --count 10

raw     snapshots of 4096 unprocessed ADC samples (16.4 ms each)
energy  one record per 1.024 ms audio block: the detector's input energy,
        the full-band energy, the noise floor and the pulse counters
verify  checks the PSRAM ring: takes a raw snapshot, reads the same blocks
        back from the PSRAM and compares them bit for bit (nothing saved)
stats   the timing and health counters (the Live Expressions ones, e.g.
        dspLoadMaxPercent). With --window S it first restarts the worst-case
        timings, waits S seconds, then reads them (nothing saved)
sdtest  copies the PSRAM ring's live audio to SBnnX.RAW on the SD card
        (nn = sectors per write, X = P polled / D DMA) while everything else
        runs, starting --pretrigger seconds behind; reports whether those
        writes keep up with recording (diary 38-40). Check the files on the
        PC afterwards with Tools/check_raw.py
fftcheck  captures FFT frames with the board's result for each (FFT_Process)
        and compares them with numpy's rfft of the same raw samples

Captures are saved as .npz files in Captures/ (beside Tools/), named with the
date, time, kind and label. Analyse them with Tools/analyse_capture.py.

Needs: python -m pip install pyserial numpy
"""

import argparse
import struct
import sys
import time
import zlib
from pathlib import Path

import numpy as np
import serial
import serial.tools.list_ports


# USB IDs of the board's virtual COM port (USB_Device/App/usbd_desc.c)
USB_VID = 0x0483
USB_PID = 0x5740

PROTOCOL_VERSION = 1
MAGIC = b"BATD"

# DebugFrameHeader_t: magic, type, version, sequence, length, arg0, arg1
HEADER = struct.Struct("<4sBBHIII")

# DebugInfo_t
INFO = struct.Struct("<IHHHHI24s")

# DebugEnergyRecord_t
ENERGY_DTYPE = np.dtype([
    ("block", "<u4"),
    ("energy", "<f4"),
    ("total", "<f4"),
    ("floor_db", "<f4"),
    ("pulses", "<u2"),
    ("noise_runs", "u1"),
    ("flags", "u1"),
])

# DebugStat_t: name, type ('u' uint32, 'b' uint8, 'f' float), pointer (ignored)
STAT_ENTRY = struct.Struct("<27scI")

# Counters that must stay 0, and limits (CLAUDE.md)
STATS_MUST_BE_ZERO = {"dacLateBlocks", "psramRingOverTcem", "psramRingLateBlocks", "sdErrorCount",
                      "sdHalFailures", "sdStaleRx",
                      "psramRingFaults", "playbackUnderruns", "recReadFailures"}
STATS_LIMITS = {"psramRingMaxSelectNs": (8000, "the PSRAM's 8 us tCEM"),
                "dspLoadMaxPercent": (100, "a whole block period")}

# SdBench_Result_t (Core/Inc/sd_bench.h)
SD_RESULT = struct.Struct("<25I8I")
SD_RESULT_FIELDS = (
    "status", "fresult", "flags", "elapsed_ms", "blocks_written", "blocks_lost",
    "read_failures", "start_backlog", "max_backlog", "catch_up_ms", "catch_up_blocks",
    "catch_up_audio_blocks", "catch_up_fft_dropped", "steady_ms", "steady_busy_us",
    "steady_audio_blocks", "steady_fft_dropped", "max_write_us", "sectors_per_write",
    "cluster_sectors", "catch_up_busy_us", "uses_dma", "sd_error", "sd_card_status",
    "sd_busy_max_us",
)

# sd_spi.h: SD_Step_t, SD_Status_t, and the commands in sdLastError
SD_STEPS = ("none", "card still busy when selected", "bad command response (R1)", "no or bad data token",
            "SPI/DMA failure receiving a block", "card still busy before the next block",
            "SPI/DMA failure sending a block", "block rejected by the card")
SD_DRIVER_STATUS = ("OK", "no card", "init failed", "timeout", "command rejected", "data rejected", "SPI error")
SD_COMMANDS = {0: "CMD0", 17: "CMD17 read", 18: "CMD18 multi read", 24: "CMD24 write",
               25: "CMD25 multi write", 0xFE: "sync"}
SD_R2_BITS = ("card locked", "WP erase skip / lock failed", "error", "card controller error",
              "card ECC failed", "write-protect violation", "erase parameter", "out of range")


def describe_sd_error(value):
    """sdLastError: step | status << 8 | reply << 16 | command << 24."""
    if value == 0:
        return "none"
    step, status, reply, command = value & 0xFF, (value >> 8) & 0xFF, (value >> 16) & 0xFF, value >> 24
    step_text = SD_STEPS[step] if step < len(SD_STEPS) else f"step {step}"
    status_text = SD_DRIVER_STATUS[status] if status < len(SD_DRIVER_STATUS) else f"status {status}"
    if step == 7:   # data response token xxx0sss1: 010 accepted, 101 CRC error, 110 write error
        reply_text = {0x0B: " (CRC error)", 0x0D: " (write error)"}.get(reply & 0x1F, "")
    else:
        reply_text = ""
    return (f"{SD_COMMANDS.get(command, f'command 0x{command:02X}')}: {step_text}, {status_text}, "
            f"reply 0x{reply:02X}{reply_text}")


def describe_card_status(value):
    """sdLastCardStatus: 0x10000 | R1 << 8 | R2 from CMD13; 0x1FFFF = no answer; 0 = not read."""
    if value == 0:
        return "not read"
    if value == 0x1FFFF:
        return "no answer to CMD13"
    r1, r2 = (value >> 8) & 0xFF, value & 0xFF
    bits = [name for i, name in enumerate(SD_R2_BITS) if r2 & (1 << i)]
    return f"R1 0x{r1:02X}, R2 0x{r2:02X}" + (f" ({', '.join(bits)})" if bits else " (no error bits)")


SD_PHASES = ("idle", "deselect", "select/busy wait", "command frame", "waiting for R1",
             "busy wait before a block", "write token", "CRC/response after a write",
             "waiting for a read token", "CRC after a read", "late-response probe")
HAL_STATUS = ("OK", "ERROR", "BUSY", "TIMEOUT")
HAL_SPI_STATE = ("RESET", "READY", "BUSY", "BUSY_TX", "BUSY_RX", "BUSY_TX_RX", "ERROR", "ABORT")


def sd_phase(phase):
    return SD_PHASES[phase] if phase < len(SD_PHASES) else f"phase {phase}"


def describe_hal_failure(value):
    """sdHalFailure: status | State << 4 | ErrorCode << 8 | Lock << 16 | phase << 17 | command << 24."""
    if value == 0:
        return "none"
    status, state, error = value & 0xF, (value >> 4) & 0xF, (value >> 8) & 0xFF
    lock, phase, command = (value >> 16) & 1, (value >> 17) & 0x7F, value >> 24
    return (f"HAL {HAL_STATUS[status] if status < 4 else status} in {sd_phase(phase)} "
            f"({SD_COMMANDS.get(command, f'command 0x{command:02X}')}): handle "
            f"{HAL_SPI_STATE[state] if state < 8 else state}, error 0x{error:02X}, {'locked' if lock else 'unlocked'}")


def describe_stale_rx(value):
    """sdStaleRxFirst: SPI2 SR | phase << 16 | command << 24."""
    if value == 0:
        return "none"
    sr, phase, command = value & 0xFFFF, (value >> 16) & 0xFF, value >> 24
    flags = [name for bit, name in ((0, "RXNE"), (6, "OVR"), (7, "BSY")) if sr & (1 << bit)]
    return (f"before {sd_phase(phase)} ({SD_COMMANDS.get(command, f'command 0x{command:02X}')}): "
            f"SR 0x{sr:04X}, FRLVL {(sr >> 9) & 3} (bytes waiting; 3 = 3 or more)"
            + (f", {' '.join(flags)}" if flags else ""))


def describe_late_response(value):
    """sdLateResponse: extra bytes before the card answered | first byte << 16; 0xFFFF = never."""
    if value == 0:
        return "not probed"
    if value == 0xFFFF:
        return "card never answered in 1024 more bytes"
    count, reply = value & 0xFFFF, (value >> 16) & 0xFF
    if reply & 0x11 == 0x01:
        kind = {0x05: "accepted", 0x0B: "CRC error", 0x0D: "write error"}.get(reply & 0x1F, "unknown")
        meaning = f"a data response ({kind})"
    elif reply == 0x00:
        meaning = "busy (the response itself was missed)"
    else:
        meaning = "not a data response"
    return f"card answered {count} byte(s) late with 0x{reply:02X}: {meaning}"
SD_STATUS = ("done", "buffer too small", "playback has the ring", "the PSRAM ring isn't recording",
             "f_open failed (no card, or not FAT)", "f_write failed, or the card is full",
             "f_close failed", "sectors per write isn't 1, 2, 4, 8 or 16")
SD_SECTOR_CHOICES = (1, 2, 4, 8, 16)    # SD_BENCH_MAX_SECTORS = 16
SD_FLAG_RING_PAUSED = 0x01
SD_FLAG_TIME_LIMIT = 0x02
SD_FLAG_NEVER_CAUGHT = 0x04
SD_HISTOGRAM_BINS = ("<1", "1-2", "2-5", "5-10", "10-50", "50-100", "100-250", ">=250")   # ms
SD_RING_BLOCKS = 16384          # PSRAM_RING_BLOCKS
SD_MAX_PRETRIGGER = 15          # DEBUG_SD_BENCH_MAX_PRETRIGGER_S

# FatFS R0.12c FRESULT codes
FRESULT = ("FR_OK", "FR_DISK_ERR", "FR_INT_ERR", "FR_NOT_READY", "FR_NO_FILE", "FR_NO_PATH",
           "FR_INVALID_NAME", "FR_DENIED", "FR_EXIST", "FR_INVALID_OBJECT", "FR_WRITE_PROTECTED",
           "FR_INVALID_DRIVE", "FR_NOT_ENABLED", "FR_NO_FILESYSTEM", "FR_MKFS_ABORTED", "FR_TIMEOUT",
           "FR_LOCKED", "FR_NOT_ENOUGH_CORE", "FR_TOO_MANY_OPEN_FILES", "FR_INVALID_PARAMETER")

MAX_PAYLOAD = 65536
CAPTURE_DIR = Path(__file__).resolve().parent.parent / "Captures"


class Frame:
    def __init__(self, kind, sequence, arg0, arg1, payload):
        self.kind = kind
        self.sequence = sequence
        self.arg0 = arg0
        self.arg1 = arg1
        self.payload = payload


class FrameReader:
    """Splits the byte stream into frames, resyncing on bad data."""

    def __init__(self, port):
        self.port = port
        self.buffer = bytearray()
        self.crc_errors = 0
        self.skipped_bytes = 0
        self.lost_frames = 0
        self.last_sequence = None

    def read_frame(self, timeout):
        deadline = time.monotonic() + timeout

        while True:
            frame = self._parse()
            if frame is not None:
                return frame
            if time.monotonic() > deadline:
                return None
            self.buffer += self.port.read(max(1, self.port.in_waiting))

    def _parse(self):
        while True:
            start = self.buffer.find(MAGIC)

            if start < 0:
                # keep a few bytes in case the magic is split across reads
                drop = max(0, len(self.buffer) - (len(MAGIC) - 1))
                self.skipped_bytes += drop
                del self.buffer[:drop]
                return None

            if start > 0:
                self.skipped_bytes += start
                del self.buffer[:start]

            if len(self.buffer) < HEADER.size:
                return None

            _, kind, version, sequence, length, arg0, arg1 = HEADER.unpack_from(self.buffer)

            if version != PROTOCOL_VERSION or length > MAX_PAYLOAD:
                self.skipped_bytes += 1
                del self.buffer[:1]
                continue

            end = HEADER.size + length
            if len(self.buffer) < end + 4:
                return None

            (crc,) = struct.unpack_from("<I", self.buffer, end)

            if zlib.crc32(self.buffer[:end]) != crc:
                self.crc_errors += 1
                del self.buffer[:1]
                continue

            payload = bytes(self.buffer[HEADER.size:end])
            del self.buffer[:end + 4]

            if self.last_sequence is not None:
                self.lost_frames += (sequence - self.last_sequence - 1) & 0xFFFF
            self.last_sequence = sequence

            return Frame(chr(kind), sequence, arg0, arg1, payload)

    def wait_for(self, kind, timeout):
        """Return the next frame of this kind, skipping others."""
        deadline = time.monotonic() + timeout

        while True:
            frame = self.read_frame(max(0.0, deadline - time.monotonic()))
            if frame is None:
                return None
            if frame.kind == kind:
                return frame


def find_port():
    for port in serial.tools.list_ports.comports():
        if port.vid == USB_VID and port.pid == USB_PID:
            return port.device
    sys.exit("Bat detector not found: is the USB-C cable in (the working way round)?")


def open_board(port_name):
    """Open the port, stop any stream and throw away whatever was queued."""
    port = serial.Serial(port_name, timeout=0.05)   # USB CDC ignores the baud rate
    port.write(b"S")

    deadline = time.monotonic() + 3.0
    quiet_since = time.monotonic()

    while time.monotonic() - quiet_since < 0.3 and time.monotonic() < deadline:
        if port.read(4096):
            quiet_since = time.monotonic()

    return port


def read_info(reader):
    reader.port.write(b"I")
    frame = reader.wait_for("I", timeout=2.0)

    if frame is None:
        sys.exit("No reply to the info request: is the debug capture built in (DEBUG_CAPTURE)?")

    rate, block_size, snapshot_blocks, record_size, fifo_records, blocks, build = \
        INFO.unpack(frame.payload)

    if record_size != ENERGY_DTYPE.itemsize:
        sys.exit(f"Energy record is {record_size} bytes on the board, "
                 f"{ENERGY_DTYPE.itemsize} here: update ENERGY_DTYPE")

    return {
        "sample_rate": rate,
        "block_size": block_size,
        "snapshot_blocks": snapshot_blocks,
        "fifo_records": fifo_records,
        "blocks": blocks,
        "build": build.split(b"\0")[0].decode("ascii", "replace"),
    }


def print_info(info):
    seconds = info["blocks"] * info["block_size"] / info["sample_rate"]
    print(f"Build {info['build']}, {info['sample_rate']} Hz, {info['block_size']}-sample blocks, "
          f"snapshot {info['snapshot_blocks'] * info['block_size']} samples, "
          f"running {seconds:.0f} s")


def capture_raw(reader, count, interval):
    snapshots = []
    start_blocks = []

    for i in range(count):
        reader.port.write(b"R")
        frame = reader.wait_for("R", timeout=2.0)

        if frame is None:
            sys.exit(f"Snapshot {i + 1} didn't arrive")

        snapshots.append(np.frombuffer(frame.payload, dtype="<u2"))
        start_blocks.append(frame.arg0)

        samples = snapshots[-1]
        print(f"  snapshot {i + 1}/{count}: codes {samples.min()}-{samples.max()}, "
              f"mean {samples.mean():.1f}, rms {samples.std():.1f}")

        if i + 1 < count:
            time.sleep(interval)

    return {"samples": np.vstack(snapshots), "start_block": np.array(start_blocks, dtype=np.uint32)}


def capture_energy(reader, seconds, block_seconds):
    chunks = []
    dropped = 0
    received = 0
    pulses_start = None
    next_report = time.monotonic() + 5.0

    reader.port.write(b"E")
    end = time.monotonic() + seconds

    while time.monotonic() < end:
        frame = reader.read_frame(timeout=0.5)

        if frame is not None and frame.kind == "E":
            records = np.frombuffer(frame.payload, dtype=ENERGY_DTYPE)
            chunks.append(records)
            received += len(records)
            dropped = frame.arg0

            if pulses_start is None and len(records):
                pulses_start = int(records["pulses"][0])

        if time.monotonic() >= next_report and chunks:
            last = chunks[-1][-1]
            pulses = (int(last["pulses"]) - pulses_start) & 0xFFFF
            print(f"  {received * block_seconds:5.1f} s recorded, energy {10 * np.log10(last['energy'] + 1e-6):5.1f} dB, "
                  f"floor {last['floor_db']:5.1f} dB, {pulses} pulses")
            next_report += 5.0

    reader.port.write(b"S")

    if not chunks:
        sys.exit("No energy records arrived")

    records = np.concatenate(chunks)
    gaps = int(np.count_nonzero(np.diff(records["block"].astype(np.int64)) != 1))

    return {name: records[name] for name in ENERGY_DTYPE.names} | {
        "dropped": np.uint32(dropped),
        "gaps": np.uint32(gaps),
    }


def verify_ring(reader, count, interval, block_size):
    """Snapshot, then the same blocks read back from the PSRAM ring: they must match exactly."""
    failures = 0

    for i in range(count):
        reader.port.write(b"R")
        snapshot = reader.wait_for("R", timeout=2.0)
        if snapshot is None:
            sys.exit(f"Snapshot {i + 1} didn't arrive")

        reader.port.write(b"M")
        copy = reader.wait_for("M", timeout=2.0)
        if copy is None:
            sys.exit("No reply to the PSRAM read-back: is this firmware built with the ring?")
        if not copy.payload:
            sys.exit("The board couldn't read the snapshot back: is the PSRAM ring running? "
                     "(the boot screen says why not)")

        adc = np.frombuffer(snapshot.payload, dtype="<u2")
        psram = np.frombuffer(copy.payload, dtype="<u2")
        bad = np.flatnonzero(adc != psram)

        if bad.size:
            failures += 1
            first = bad[0]
            print(f"  {i + 1}/{count}: ring block {copy.arg0}: {bad.size} of {adc.size} samples differ, "
                  f"first at sample {first} (block {first // block_size}): ADC {adc[first]}, PSRAM {psram[first]}")
        else:
            print(f"  {i + 1}/{count}: ring block {copy.arg0}: all {adc.size} samples match")

        if i + 1 < count:
            time.sleep(interval)

    compared = count * len(adc)
    if failures:
        print(f"FAIL: {failures} of {count} snapshots differed")
    else:
        print(f"PASS: {count} snapshots, {compared} samples ({2 * compared / 1024:.0f} KB) identical")
    return failures == 0


# fftcheck passes if every bin is within this fraction of the frame's largest bin.
# float32 rounding through 9 FFT stages is ~1e-6 of the peak; 1e-4 is -80 dB.
FFT_CHECK_LIMIT = 1e-4


def fft_check(reader, info, count, interval):
    """Board FFT (FFT_Process) vs numpy rfft on the same frame of raw ADC codes."""
    worst = 0.0
    raws, boards, refs = [], [], []

    for i in range(count):
        reader.port.write(b"F")
        frame = reader.wait_for("F", timeout=2.0)
        if frame is None:
            sys.exit("No FFT frame arrived: is the board running firmware with the FFT capture ('F')?")

        n, bins = frame.arg0, frame.arg1
        raw = np.frombuffer(frame.payload[:2 * n], dtype="<u2")
        board = np.frombuffer(frame.payload[2 * n:2 * n + 4 * bins], dtype="<f4").astype(np.float64)

        # As fft.c: (code - 2048) / 2048, times the symmetric Hann window from
        # Tools/gen_dsp_tables.py, then an unnormalised real FFT
        window = 0.5 * (1.0 - np.cos(2.0 * np.pi * np.arange(n) / (n - 1)))
        reference = np.abs(np.fft.rfft((raw.astype(np.float64) - 2048.0) / 2048.0 * window))

        error = np.abs(board - reference)
        relative = error.max() / reference.max()
        worst = max(worst, relative)
        peak = int(np.argmax(reference[2:])) + 2       # past the DC bins

        print(f"  frame {i + 1}/{count}: codes {raw.min()}-{raw.max()}, strongest bin {peak} "
              f"({peak * info['sample_rate'] / n / 1000:.1f} kHz): board {board[peak]:.4f}, "
              f"numpy {reference[peak]:.4f}; worst bin {int(error.argmax())} off by "
              f"{relative:.1e} of the largest bin ({20 * np.log10(max(relative, 1e-12)):.0f} dB)")

        raws.append(raw)
        boards.append(board)
        refs.append(reference)

        if i + 1 < count:
            time.sleep(interval)

    passed = worst < FFT_CHECK_LIMIT
    print(f"{'PASS' if passed else 'FAIL'}: worst error {worst:.1e} of the largest bin over {count} frames "
          f"(limit {FFT_CHECK_LIMIT:.0e})")
    return passed, {"raw": np.vstack(raws), "board": np.vstack(boards), "numpy": np.vstack(refs)}


def ratio(numerator, denominator):
    return numerator / denominator if denominator else float("nan")


def percent(fraction):
    return "n/a" if fraction != fraction else f"{fraction:.0%}"


def read_stat_table(reader):
    """Names and types of the board's timing and health counters ('N')."""
    reader.port.write(b"N")
    frame = reader.wait_for("N", timeout=2.0)

    if frame is None:
        sys.exit("No reply to the stats request: is the board running firmware with the stats table?")
    if frame.arg1 != STAT_ENTRY.size:
        sys.exit(f"Stat entries are {frame.arg1} bytes on the board, {STAT_ENTRY.size} here: update STAT_ENTRY")

    table = []
    for i in range(frame.arg0):
        name, kind, _ = STAT_ENTRY.unpack_from(frame.payload, i * STAT_ENTRY.size)
        table.append((name.split(b"\0")[0].decode("ascii"), kind.decode("ascii")))
    return table


def read_stats(reader, table):
    """Current values ('T'), as {name: value}, and the board's uptime in ms."""
    reader.port.write(b"T")
    frame = reader.wait_for("T", timeout=2.0)

    if frame is None:
        sys.exit("No reply to the stats read")
    if frame.arg0 != len(table):
        sys.exit("The board's stats table doesn't match the names it sent")

    values = {}
    for i, (name, kind) in enumerate(table):
        raw = frame.payload[4 * i:4 * i + 4]
        values[name] = struct.unpack("<f", raw)[0] if kind == "f" else struct.unpack("<I", raw)[0]
    return values, frame.arg1


def check_stat(name, value):
    if name in STATS_MUST_BE_ZERO and value != 0:
        return "<-- must stay 0"
    if name in STATS_LIMITS and value >= STATS_LIMITS[name][0]:
        return f"<-- at or over {STATS_LIMITS[name][1]}"
    return ""


def describe_stat(name, value, clock, block_seconds):
    """The value in more useful units, where there are some."""
    if name == "SystemCoreClock":
        return f"{value / 1e6:.0f} MHz"
    if name == "blockCount":
        return f"{value * block_seconds:.0f} s of audio"
    if name == "dspCyclesMax":
        return f"{value / clock * 1e6:.0f} us"
    if name.startswith("fftProcessCycles"):
        return f"{value / clock * 1e3:.2f} ms"
    if name.endswith("Ns"):
        return f"{value / 1000:.2f} us"
    if name == "sdBusyMaxUs":
        return f"{value / 1000:.1f} ms"
    if name == "sdLastError":
        return describe_sd_error(value)
    if name == "sdLastCardStatus":
        return describe_card_status(value)
    if name == "sdLateResponse":
        return describe_late_response(value)
    if name == "sdHalFailure":
        return describe_hal_failure(value)
    if name == "sdStaleRxFirst":
        return describe_stale_rx(value)
    return ""


def print_stats(values, uptime_ms, info):
    clock = values.get("SystemCoreClock") or 170_000_000
    block_seconds = info["block_size"] / info["sample_rate"]
    problems = 0

    print(f"Uptime {uptime_ms / 1000:.0f} s")
    for name, value in values.items():
        shown = f"{value:.1f}" if isinstance(value, float) else str(value)
        note = check_stat(name, value)
        problems += bool(note)
        print(f"  {name:<22} {shown:>10}  {describe_stat(name, value, clock, block_seconds):<16}{note}")

    print("All counters within their limits" if not problems else f"{problems} counter(s) out of limits")


def sd_test(reader, info, seconds, pretrigger, sectors, dma):
    """Run the board's SD write test (sd_bench.c) once; return (result, data to save)."""
    table = read_stat_table(reader)

    # Baseline: how much the FFT drops with no SD traffic
    first, _ = read_stats(reader, table)
    time.sleep(2.0)
    baseline, _ = read_stats(reader, table)
    baseline_drop = ratio(baseline["fftDroppedBlocks"] - first["fftDroppedBlocks"],
                          baseline["blockCount"] - first["blockCount"])

    reader.port.write(b"Z")         # so the worst-case timings cover the test only
    reader.port.write(b"W" + bytes([pretrigger, sectors, dma]))
    print(f"SD write test: {sectors} sector(s) per write, {'DMA' if dma else 'polled'}, {seconds:.0f} s, "
          f"starting {pretrigger} s behind the ring")

    frame = None
    start = time.monotonic()
    next_report = start + 10.0

    while time.monotonic() - start < seconds:
        received = reader.read_frame(timeout=0.5)
        if received is not None and received.kind == "W":
            frame = received        # ended early: an error, or the board's time limit
            break
        if time.monotonic() >= next_report:
            print(f"  {time.monotonic() - start:3.0f} s")
            next_report += 10.0

    if frame is None:
        reader.port.write(b"S")
        frame = reader.wait_for("W", timeout=5.0)
        if frame is None:
            sys.exit("The board didn't send the SD test's result")

    if len(frame.payload) != SD_RESULT.size:
        sys.exit(f"SD test result is {len(frame.payload)} bytes, {SD_RESULT.size} expected: update SD_RESULT")

    after, _ = read_stats(reader, table)

    values = SD_RESULT.unpack(frame.payload)
    result = dict(zip(SD_RESULT_FIELDS, values))
    result["histogram"] = np.array(values[len(SD_RESULT_FIELDS):], dtype=np.uint32)

    summary = print_sd_result(result, info, baseline_drop, after)

    names = [name for name, _ in table]
    return summary, {key: np.asarray(value) for key, value in result.items()} | {
        "pretrigger_s": np.uint32(pretrigger),
        "baseline_fft_drop": np.float64(baseline_drop),
        "stat_names": np.array(names),
        "stats_after": np.array([float(after[name]) for name in names]),
    }


def print_sd_result(r, info, baseline_drop, stats):
    audio_rate = info["sample_rate"] * 2            # bytes/s that recording writes
    block_bytes = info["block_size"] * 2
    block_seconds = info["block_size"] / info["sample_rate"]
    never_caught = bool(r["flags"] & SD_FLAG_NEVER_CAUGHT)
    sectors = r["sectors_per_write"]
    transfer = "DMA" if r["uses_dma"] else "polled"
    summary = {"sectors": sectors, "dma": transfer, "rate": None, "headroom": None, "busy": None,
               "slowest_ms": None, "lost": r["blocks_lost"], "fft_drop": None, "verdict": "no verdict"}

    status = SD_STATUS[r["status"]] if r["status"] < len(SD_STATUS) else f"status {r['status']}"
    if r["status"] != 0:
        error = FRESULT[r["fresult"]] if r["fresult"] < len(FRESULT) else f"FRESULT {r['fresult']}"
        print(f"SD test FAILED: {status}" + (f" ({error})" if r["fresult"] else ""))
        print(f"  SD driver: {describe_sd_error(r['sd_error'])}")
        print(f"  Card status after the failure: {describe_card_status(r['sd_card_status'])}")
        print(f"  Late-response probe: {describe_late_response(stats.get('sdLateResponse', 0))}")
        print(f"  SD errors since boot: {stats.get('sdErrorCount', 'n/a')}")
        print(f"  First failed 1-byte HAL transfer: {describe_hal_failure(stats.get('sdHalFailure', 0))}")
        print(f"  First leftover receive data: {describe_stale_rx(stats.get('sdStaleRxFirst', 0))}")
        summary["verdict"] = f"failed: {status}"
        if r["blocks_written"] == 0:
            return summary

    # sd_bench.c SD_BENCH_FILE_FORMAT
    file_name = f"SB{sectors:02d}{'D' if r['uses_dma'] else 'P'}.RAW"
    print(f"SD write test, {sectors} sector(s) per f_write, {transfer}: {status} after "
          f"{r['elapsed_ms'] / 1000:.1f} s, {r['blocks_written']} blocks "
          f"({r['blocks_written'] * block_bytes / 1e6:.1f} MB) written to {file_name}")
    print(f"  Longest wait for the card to stop being busy: {r['sd_busy_max_us'] / 1000:.1f} ms "
          f"(the driver gives up at 500 ms)")
    print(f"  Driver self-checks: {stats.get('sdHalFailures', 'n/a')} failed 1-byte HAL transfers "
          f"(first: {describe_hal_failure(stats.get('sdHalFailure', 0))}); "
          f"{stats.get('sdStaleRx', 'n/a')} times leftover receive data "
          f"(first: {describe_stale_rx(stats.get('sdStaleRxFirst', 0))})")
    recoveries = stats.get("sdRecoveries", 0)
    print(f"  Recoveries (card restarted and the transfer retried): {recoveries}, "
          f"{stats.get('sdRecoveryFailures', 0)} of them failed"
          + (f"; first failure: {describe_sd_error(stats.get('sdLastError', 0))}, "
             f"probe: {describe_late_response(stats.get('sdLateResponse', 0))}" if recoveries else ""))
    print(f"  Card clusters: {r['cluster_sectors']} sectors ({r['cluster_sectors'] * block_bytes // 1024} KB)"
          + ("" if r["cluster_sectors"] % sectors == 0 else ": smaller than a write, so FatFS splits some writes"))
    if sectors == SD_SECTOR_CHOICES[-1]:
        print(f"  Note: at {sectors} sectors the whole buffer is one write, so the next blocks can't load "
              "from the ring while it's being written")
    if r["flags"] & SD_FLAG_TIME_LIMIT:
        print("  Stopped by the board's 600 s limit")
    if r["flags"] & SD_FLAG_RING_PAUSED:
        print("  WARNING: the ring paused during the test (PLAYBACK pressed?), so these figures are off")

    headroom_catch = None
    if r["start_backlog"] and r["catch_up_ms"]:
        rate = r["catch_up_blocks"] * block_bytes / (r["catch_up_ms"] / 1000)
        headroom_catch = rate / audio_rate
        caught = (f"not cleared in {r['catch_up_ms'] / 1000:.1f} s" if never_caught
                  else f"cleared in {r['catch_up_ms'] / 1000:.1f} s")
        print(f"  Catching up {r['start_backlog'] * block_seconds:.1f} s of pre-trigger: {caught}, at "
              f"{rate / 1000:.0f} kB/s = {headroom_catch:.2f}x the {audio_rate / 1000:.0f} kB/s recording needs")
        if r["catch_up_blocks"]:
            print(f"    inside f_write {r['catch_up_busy_us'] / (r['catch_up_ms'] * 1000):.0%} of that time, "
                  f"{r['catch_up_busy_us'] / r['catch_up_blocks'] / 1000:.3f} ms per sector")
        summary["rate"] = rate

    headroom_steady = None
    if r["steady_ms"]:
        busy = r["steady_busy_us"] / (r["steady_ms"] * 1000)
        headroom_steady = 1 / busy if busy else float("inf")
        steady_blocks = r["blocks_written"] - r["catch_up_blocks"]
        per_sector = f", {r['steady_busy_us'] / steady_blocks / 1000:.3f} ms per sector" if steady_blocks else ""
        print(f"  Once caught up ({r['steady_ms'] / 1000:.1f} s): inside f_write {busy:.0%} of the time{per_sector}, "
              f"about {headroom_steady:.2f}x headroom")
        summary["busy"] = busy

    print(f"  Backlog: at most {r['max_backlog']} blocks ({r['max_backlog'] * block_seconds:.2f} s) of the ring's "
          f"{SD_RING_BLOCKS * block_seconds:.1f} s; {r['blocks_lost']} blocks lost, "
          f"{r['read_failures']} ring read failures")

    print(f"  f_write: {int(r['histogram'].sum())} calls of {sectors} sector(s), "
          f"slowest {r['max_write_us'] / 1000:.1f} ms")
    print("    ms     " + "".join(f"{label:>9}" for label in SD_HISTOGRAM_BINS))
    print("    calls  " + "".join(f"{int(count):>9}" for count in r["histogram"]))
    summary["slowest_ms"] = r["max_write_us"] / 1000

    catch_drop = ratio(r["catch_up_fft_dropped"], r["catch_up_audio_blocks"])
    steady_drop = ratio(r["steady_fft_dropped"], r["steady_audio_blocks"])
    print(f"  FFT blocks dropped (the waterfall's input): {percent(baseline_drop)} before the test, "
          f"{percent(catch_drop)} while catching up, {percent(steady_drop)} once caught up")
    summary["fft_drop"] = steady_drop if steady_drop == steady_drop else catch_drop

    clock = stats.get("SystemCoreClock") or 170_000_000
    print(f"  During the test: DSP worst case {stats['dspLoadMaxPercent']}% of a block, "
          f"FFT worst case {stats['fftProcessCyclesMax'] / clock * 1e3:.2f} ms, "
          f"PSRAM CE# worst case {stats['psramRingMaxSelectNs'] / 1000:.2f} us")
    for name, value in stats.items():
        note = check_stat(name, value)
        if note:
            print(f"  {name} = {value} {note}")

    # Thresholds agreed in diary 38
    headroom = headroom_catch if headroom_catch is not None else headroom_steady
    writes = "single-sector writes" if sectors == 1 else f"{sectors}-sector writes"
    if r["status"] != 0 or headroom is None:
        verdict = summary["verdict"]
    elif r["blocks_lost"] or headroom < 1.0:
        verdict = f"{headroom:.2f}x: {writes} can't keep up under recording load"
    elif never_caught:
        verdict = (f"{headroom:.2f}x, but the pre-trigger wasn't cleared in time: "
                   f"run longer (needs ~{r['start_backlog'] * block_seconds / max(headroom - 1, 0.01):.0f} s)")
    elif headroom < 1.6:
        verdict = f"{headroom:.2f}x: {writes} keep up, but the margin is thin"
    else:
        verdict = f"{headroom:.2f}x: {writes} are enough for recording"
    print(f"Verdict: {verdict}")

    summary["headroom"] = headroom
    summary["verdict"] = verdict
    return summary


def print_sd_summary(summaries):
    """One line per sectors-per-write setting, after a sweep."""
    def show(value, width, spec, unit=""):
        text = "n/a" if value is None or value != value else format(value, spec) + unit
        return f"{text:>{width}}"

    print("\nSectors  Transfer  kB/s catching up  headroom  f_write busy once caught up  slowest  lost  FFT dropped")
    for s in summaries:
        rate = None if s["rate"] is None else s["rate"] / 1000
        print(f"{s['sectors']:>7}  {s['dma']:>8}  {show(rate, 16, '.0f')}  {show(s['headroom'], 8, '.2f', 'x')}  "
              f"{show(s['busy'], 27, '.0%')}  {show(s['slowest_ms'], 7, '.1f', ' ms')}  {s['lost']:>4}  "
              f"{show(s['fft_drop'], 11, '.0%')}")


def save(kind, label, info, data):
    CAPTURE_DIR.mkdir(exist_ok=True)
    name = time.strftime("%Y%m%d_%H%M%S") + f"_{kind}" + (f"_{label}" if label else "") + ".npz"
    path = CAPTURE_DIR / name

    np.savez_compressed(
        path,
        kind=kind,
        label=label or "",
        build=info["build"],
        sample_rate=info["sample_rate"],
        block_size=info["block_size"],
        **data,
    )
    return path


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("command", choices=["info", "raw", "energy", "verify", "stats", "sdtest", "fftcheck"])
    parser.add_argument("--port", help="COM port (default: find the board by its USB IDs)")
    parser.add_argument("--label", default="", help="short note for the file name, e.g. quiet-room")
    parser.add_argument("--count", type=int, default=20, help="raw, verify, fftcheck: number of snapshots (default 20)")
    parser.add_argument("--interval", type=float, default=0.25, help="raw, verify, fftcheck: seconds between snapshots")
    parser.add_argument("--seconds", type=float, default=60.0, help="energy, sdtest: how long to run")
    parser.add_argument("--window", type=float, default=0.0,
                        help="stats: restart the worst-case timings, wait this long, then read")
    parser.add_argument("--pretrigger", type=int, default=5,
                        help=f"sdtest: start this many seconds behind the ring, 0-{SD_MAX_PRETRIGGER} (default 5)")
    parser.add_argument("--sectors", type=int, nargs="+", default=[1],
                        help="sdtest: sectors per write, 1 2 4 8 or 16; several run one after another (default 1)")
    parser.add_argument("--dma", type=int, nargs="+", default=[0], choices=[0, 1],
                        help="sdtest: 0 = polled data blocks (the driver's default), 1 = DMA; "
                             "both runs each setting both ways (default 0)")
    args = parser.parse_args()

    if not 0 <= args.pretrigger <= SD_MAX_PRETRIGGER:
        sys.exit(f"--pretrigger must be 0-{SD_MAX_PRETRIGGER} s")
    if any(n not in SD_SECTOR_CHOICES for n in args.sectors):
        sys.exit(f"--sectors must be from {SD_SECTOR_CHOICES}")

    port_name = args.port or find_port()
    print(f"Board on {port_name}")

    with open_board(port_name) as port:
        reader = FrameReader(port)
        info = read_info(reader)
        print_info(info)

        if args.command == "verify":
            passed = verify_ring(reader, args.count, args.interval, info["block_size"])
            port.write(b"S")
            sys.exit(0 if passed else 1)

        if args.command == "fftcheck":
            passed, data = fft_check(reader, info, args.count, args.interval)
            port.write(b"S")
            print(f"Saved {save('fftcheck', args.label, info, data)}")
            sys.exit(0 if passed else 1)

        if args.command == "stats":
            table = read_stat_table(reader)
            if args.window > 0:
                port.write(b"Z")
                print(f"Worst-case timings restarted; measuring for {args.window:.0f} s")
                time.sleep(args.window)
            values, uptime_ms = read_stats(reader, table)
            print_stats(values, uptime_ms, info)
            return

        if args.command == "sdtest":
            summaries = []
            runs = [(sectors, dma) for dma in args.dma for sectors in args.sectors]
            for i, (sectors, dma) in enumerate(runs):
                if i:
                    print()
                summary, data = sd_test(reader, info, args.seconds, args.pretrigger, sectors, dma)
                summaries.append(summary)
                label = f"{args.label}-" if args.label else ""
                name = f"{label}{sectors}sect-{'dma' if dma else 'polled'}"
                print(f"Saved {save('sdtest', name, info, data)}")
                # the driver retries once after re-initialising the card, so a failure
                # that still gets through means the card is in trouble: stop
                if int(data["status"]) in (4, 5, 6) and i + 1 < len(runs):
                    print("Stopping the sweep: the SD card failed even after a restart")
                    break
            if len(summaries) > 1:
                print_sd_summary(summaries)
            port.write(b"S")
            return

        if args.command == "raw":
            data = capture_raw(reader, args.count, args.interval)
        elif args.command == "energy":
            block_seconds = info["block_size"] / info["sample_rate"]
            data = capture_energy(reader, args.seconds, block_seconds)
            print(f"  {len(data['block'])} records, {int(data['dropped'])} dropped on the board, "
                  f"{int(data['gaps'])} gaps in the block numbers")
        else:
            return

        port.write(b"S")
        path = save(args.command, args.label, info, data)

    print(f"Saved {path}")
    if reader.crc_errors or reader.lost_frames:
        print(f"Link errors: {reader.crc_errors} bad CRCs, {reader.lost_frames} lost frames")


if __name__ == "__main__":
    main()
