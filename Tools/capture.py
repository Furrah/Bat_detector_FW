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
    python -I Tools/capture.py fftcheck --count 10

raw     snapshots of 4096 unprocessed ADC samples (16.4 ms each)
energy  one record per 1.024 ms audio block: the detector's input energy,
        the full-band energy, the noise floor and the pulse counters
verify  checks the PSRAM ring: takes a raw snapshot, reads the same blocks
        back from the PSRAM and compares them bit for bit (nothing saved)
stats   the timing and health counters (the Live Expressions ones, e.g.
        dspLoadMaxPercent). With --window S it first restarts the worst-case
        timings, waits S seconds, then reads them (nothing saved)
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

# sd_spi.h: SD_Step_t, SD_Status_t, and the commands in sdLastError
SD_STEPS = ("none", "card still busy when selected", "bad command response (R1)", "no or bad data token",
            "SPI failure receiving a block", "card still busy before the next block",
            "SPI failure sending a block", "block rejected by the card")
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
    parser.add_argument("command", choices=["info", "raw", "energy", "verify", "stats", "fftcheck"])
    parser.add_argument("--port", help="COM port (default: find the board by its USB IDs)")
    parser.add_argument("--label", default="", help="short note for the file name, e.g. quiet-room")
    parser.add_argument("--count", type=int, default=20, help="raw, verify, fftcheck: number of snapshots (default 20)")
    parser.add_argument("--interval", type=float, default=0.25, help="raw, verify, fftcheck: seconds between snapshots")
    parser.add_argument("--seconds", type=float, default=60.0, help="energy: how long to run")
    parser.add_argument("--window", type=float, default=0.0,
                        help="stats: restart the worst-case timings, wait this long, then read")
    args = parser.parse_args()

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
