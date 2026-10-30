"""
capture.py - record debug data from the bat detector over its USB serial port.

The board side is Core/Src/debug_link.c; its frame format is described at the
top of that file, and the structs below must match it.

    python -I Tools/capture.py info
    python -I Tools/capture.py raw --count 20 --label quiet-room
    python -I Tools/capture.py energy --seconds 60 --label quiet-room

raw     snapshots of 4096 unprocessed ADC samples (16.4 ms each)
energy  one record per 1.024 ms audio block: the detector's input energy,
        the full-band energy, the noise floor and the pulse counters

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
    parser.add_argument("command", choices=["info", "raw", "energy"])
    parser.add_argument("--port", help="COM port (default: find the board by its USB IDs)")
    parser.add_argument("--label", default="", help="short note for the file name, e.g. quiet-room")
    parser.add_argument("--count", type=int, default=20, help="raw: number of snapshots (default 20)")
    parser.add_argument("--interval", type=float, default=0.25, help="raw: seconds between snapshots")
    parser.add_argument("--seconds", type=float, default=60.0, help="energy: how long to record")
    args = parser.parse_args()

    port_name = args.port or find_port()
    print(f"Board on {port_name}")

    with open_board(port_name) as port:
        reader = FrameReader(port)
        info = read_info(reader)
        print_info(info)

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
