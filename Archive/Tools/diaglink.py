"""
diaglink.py - the PC side of Core/Diag/diag_link.c: reads the board's framed
replies from a serial port, and its stats table.

    reader = FrameReader(port, magic=b"BATD", version=1)
    port.write(b"I")
    frame = reader.wait_for("I", timeout=2.0)

    table = read_stat_table(reader)          # names and types ('N')
    values, uptime_ms = read_stats(reader, table)   # current values ('T')

Frames (little-endian): a 20-byte header (magic, type, version, sequence,
payload length, arg0, arg1), the payload, then the CRC-32 of both, as
zlib.crc32 computes it. The reader resyncs on the magic after bad data and
counts CRC errors, skipped bytes and lost frames (sequence gaps).

Needs: python -m pip install pyserial
"""

import struct
import time
import zlib

import serial
import serial.tools.list_ports


HEADER = struct.Struct("<4sBBHIII")         # diag_link.c DiagFrameHeader_t
STAT_ENTRY = struct.Struct("<27scI")        # diag_stats.h DiagStat_t: name, type, (address)
MAX_PAYLOAD = 65536


class LinkError(Exception):
    pass


class Frame:
    def __init__(self, kind, sequence, arg0, arg1, payload):
        self.kind = kind
        self.sequence = sequence
        self.arg0 = arg0
        self.arg1 = arg1
        self.payload = payload


class FrameReader:
    """Splits the byte stream into frames, resyncing on bad data."""

    def __init__(self, port, magic, version):
        self.port = port
        self.magic = magic
        self.version = version
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
            start = self.buffer.find(self.magic)

            if start < 0:
                # keep a few bytes in case the magic is split across reads
                drop = max(0, len(self.buffer) - (len(self.magic) - 1))
                self.skipped_bytes += drop
                del self.buffer[:drop]
                return None

            if start > 0:
                self.skipped_bytes += start
                del self.buffer[:start]

            if len(self.buffer) < HEADER.size:
                return None

            _, kind, version, sequence, length, arg0, arg1 = HEADER.unpack_from(self.buffer)

            if version != self.version or length > MAX_PAYLOAD:
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
        """The next frame of this kind, skipping others; None on a timeout."""
        deadline = time.monotonic() + timeout

        while True:
            frame = self.read_frame(max(0.0, deadline - time.monotonic()))
            if frame is None:
                return None
            if frame.kind == kind:
                return frame


def find_port(vid, pid):
    """The serial port of a USB device with this vendor and product ID, or None."""
    for port in serial.tools.list_ports.comports():
        if port.vid == vid and port.pid == pid:
            return port.device
    return None


def open_port(name, stop=b"S"):
    """Open the port, send the stop command and throw away whatever was queued."""
    port = serial.Serial(name, timeout=0.05)    # USB CDC ignores the baud rate
    if stop:
        port.write(stop)

    deadline = time.monotonic() + 3.0
    quiet_since = time.monotonic()

    while time.monotonic() - quiet_since < 0.3 and time.monotonic() < deadline:
        if port.read(4096):
            quiet_since = time.monotonic()

    return port


def read_stat_table(reader):
    """Names and types of the board's counters ('N'), as [(name, type)]."""
    reader.port.write(b"N")
    frame = reader.wait_for("N", timeout=2.0)

    if frame is None:
        raise LinkError("no reply to the stats request ('N')")
    if frame.arg1 != STAT_ENTRY.size:
        raise LinkError(f"stat entries are {frame.arg1} bytes on the board, {STAT_ENTRY.size} here")

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
        raise LinkError("no reply to the stats read ('T')")
    if frame.arg0 != len(table):
        raise LinkError("the board's stats table doesn't match the names it sent")

    values = {}
    for i, (name, kind) in enumerate(table):
        raw = frame.payload[4 * i:4 * i + 4]
        values[name] = struct.unpack("<f", raw)[0] if kind == "f" else struct.unpack("<I", raw)[0]
    return values, frame.arg1
