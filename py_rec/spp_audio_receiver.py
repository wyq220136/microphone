#!/usr/bin/env python3
"""Receive the singlemic Classic Bluetooth SPP stream and save WAV files.

This receiver opens the Windows virtual COM port created for the paired ESP32
SPP service. The COM port is a wireless Bluetooth connection; no USB cable is
required.

Example:
  python spp_audio_receiver.py --output spp_capture.wav
"""

from __future__ import annotations

import argparse
import heapq
import math
import queue
import re
import struct
import sys
import time
import threading
import wave
from datetime import datetime
from pathlib import Path
from typing import Optional

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    serial = None
    list_ports = None


MAGIC = b"\xA5\x5A"
PACKET_DATA = 1
PACKET_START = 2
PACKET_END = 3
PACKET_DATA_HUFFMAN_LEGACY = 0x81
PACKET_DATA_HUFFMAN = 0x83
PACKET_DATA_BLOCK_DELTA = 0x82
DATA_TYPES = {PACKET_DATA, PACKET_DATA_HUFFMAN, PACKET_DATA_HUFFMAN_LEGACY,
              PACKET_DATA_BLOCK_DELTA}
PACKET_TYPES = DATA_TYPES | {PACKET_START, PACKET_END}
MAX_FRAMES_PER_PACKET = 76
MAX_PAYLOAD_BYTES = MAX_FRAMES_PER_PACKET * 3
HEADER = struct.Struct("<2sBBHHIH")
CRC = struct.Struct("<H")
CRC_BYTES = CRC.size
DEFAULT_BLUETOOTH_ADDRESS = "88:F1:55:B6:9D:DE"


def crc16_ccitt(data: bytes | bytearray | memoryview) -> int:
    crc = 0xFFFF
    for value in data:
        crc ^= value << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (
                crc << 1
            ) & 0xFFFF
    return crc


class ReceiveRateStats:
    def __init__(self, interval: float) -> None:
        self.interval = interval
        self.started = time.monotonic()
        self.raw_bytes = 0
        self.protocol_bytes = 0
        self.payload_bytes = 0
        self.packets = 0

    def update(self, raw_bytes: int, packets: list[bytes],
               reader_status: str = "") -> None:
        self.raw_bytes += raw_bytes
        self.protocol_bytes += sum(len(packet) for packet in packets)
        self.payload_bytes += sum(max(0, len(packet) - HEADER.size - CRC_BYTES)
                                  for packet in packets)
        self.packets += len(packets)

        now = time.monotonic()
        elapsed = now - self.started
        if elapsed < self.interval:
            return

        print(
            f"Bluetooth RX: {self.raw_bytes / elapsed:.0f} B/s "
            f"({self.raw_bytes / elapsed / 1024:.2f} KiB/s), "
            f"protocol={self.protocol_bytes / elapsed:.0f} B/s, "
            f"payload={self.payload_bytes / elapsed:.0f} B/s, "
            f"packets={self.packets / elapsed:.1f}/s {reader_status}"
        )
        self.started = now
        self.raw_bytes = 0
        self.protocol_bytes = 0
        self.payload_bytes = 0
        self.packets = 0


def _huffman_frequency(symbol: int) -> int:
    distance = min(symbol, 255 - symbol)
    radius = 1 + distance
    return 1 + 65535 // (radius * radius)


def _build_huffman_codes() -> dict[int, tuple[int, int]]:
    heap = [
        (_huffman_frequency(symbol), symbol, symbol, None, None)
        for symbol in range(256)
    ]
    heapq.heapify(heap)
    next_node = 256
    while len(heap) > 1:
        left = heapq.heappop(heap)
        right = heapq.heappop(heap)
        heapq.heappush(
            heap,
            (left[0] + right[0], next_node, None, left, right),
        )
        next_node += 1

    lengths: dict[int, int] = {}

    def visit(node: tuple, depth: int) -> None:
        symbol = node[2]
        if symbol is not None:
            lengths[symbol] = max(depth, 1)
            return
        visit(node[3], depth + 1)
        visit(node[4], depth + 1)

    visit(heap[0], 0)
    codes: dict[int, tuple[int, int]] = {}
    code = 0
    previous_length = 0
    for length in range(1, max(lengths.values()) + 1):
        for symbol in range(256):
            if lengths[symbol] != length:
                continue
            code <<= length - previous_length
            codes[symbol] = (code, length)
            code += 1
            previous_length = length
    return codes


HUFFMAN_CODES = _build_huffman_codes()
HUFFMAN_DECODE = {(length, code): symbol
                  for symbol, (code, length) in HUFFMAN_CODES.items()}
HUFFMAN_MAX_BITS = max(length for _, length in HUFFMAN_CODES.values())


def decode_legacy_huffman_pcm24(payload: bytes, frames: int) -> bytes:
    low_byte_count = frames * 2
    bitstream = payload[low_byte_count:]
    if len(payload) <= low_byte_count or len(bitstream) >= frames:
        raise ValueError("invalid Huffman payload size")

    bit_position = 0
    decoded = bytearray(frames * 3)
    for frame in range(frames):
        code = 0
        high_byte = None
        for length in range(1, HUFFMAN_MAX_BITS + 1):
            if bit_position >= len(bitstream) * 8:
                raise ValueError("truncated Huffman bitstream")
            bit = (bitstream[bit_position // 8] >>
                   (7 - bit_position % 8)) & 1
            bit_position += 1
            code = (code << 1) | bit
            high_byte = HUFFMAN_DECODE.get((length, code))
            if high_byte is not None:
                break
        if high_byte is None:
            raise ValueError("invalid Huffman code")

        source_offset = frame * 2
        target_offset = frame * 3
        decoded[target_offset] = payload[source_offset]
        decoded[target_offset + 1] = payload[source_offset + 1]
        decoded[target_offset + 2] = high_byte

    if (bit_position + 7) // 8 != len(bitstream):
        raise ValueError("unexpected trailing Huffman bytes")
    padding_bits = len(bitstream) * 8 - bit_position
    if padding_bits and bitstream[-1] & ((1 << padding_bits) - 1):
        raise ValueError("nonzero Huffman padding")
    return bytes(decoded)


def decode_huffman_pcm24(payload: bytes, frames: int) -> bytes:
    """Decode 0x83: all PCM24 bytes, in sample/little-endian byte order."""
    if not 0 < frames <= MAX_FRAMES_PER_PACKET or not 0 < len(payload) < frames * 3:
        raise ValueError("invalid Huffman payload size")
    decoded = bytearray(frames * 3)
    bit_position = 0
    for index in range(len(decoded)):
        code = 0
        for length in range(1, HUFFMAN_MAX_BITS + 1):
            if bit_position >= len(payload) * 8:
                raise ValueError("truncated Huffman bitstream")
            code = (code << 1) | ((payload[bit_position // 8] >>
                                   (7 - bit_position % 8)) & 1)
            bit_position += 1
            symbol = HUFFMAN_DECODE.get((length, code))
            if symbol is not None:
                decoded[index] = symbol
                break
        else:
            raise ValueError("invalid Huffman code")
    if (bit_position + 7) // 8 != len(payload):
        raise ValueError("unexpected trailing Huffman bytes")
    padding_bits = len(payload) * 8 - bit_position
    if padding_bits and payload[-1] & ((1 << padding_bits) - 1):
        raise ValueError("nonzero Huffman padding")
    return bytes(decoded)


def decode_block_delta_pcm24(payload: bytes, frames: int) -> bytes:
    if frames < 2 or len(payload) < 4:
        raise ValueError("invalid block-delta payload size")

    bit_width = payload[0]
    if bit_width < 1 or bit_width > 25:
        raise ValueError("invalid block-delta bit width")
    bit_count = (frames - 1) * bit_width
    expected_size = 4 + (bit_count + 7) // 8
    if len(payload) != expected_size:
        raise ValueError("unexpected block-delta payload length")

    anchor_wire = int.from_bytes(payload[1:4], "little")
    anchor = anchor_wire - (1 << 24) if anchor_wire & (1 << 23) else anchor_wire
    decoded = bytearray(frames * 3)
    decoded[0:3] = payload[1:4]

    bitstream = payload[4:]
    bit_position = 0
    sign_bit = 1 << (bit_width - 1)
    sign_range = 1 << bit_width
    for frame in range(1, frames):
        encoded_delta = 0
        for bit in range(bit_width):
            source_bit = (
                bitstream[bit_position // 8] >> (bit_position % 8)
            ) & 1
            encoded_delta |= source_bit << bit
            bit_position += 1
        delta = (
            encoded_delta - sign_range
            if encoded_delta & sign_bit
            else encoded_delta
        )
        sample = anchor + delta
        if sample < -(1 << 23) or sample > (1 << 23) - 1:
            raise ValueError("block-delta sample outside PCM24 range")
        target_offset = frame * 3
        decoded[target_offset:target_offset + 3] = (
            sample & 0xFFFFFF
        ).to_bytes(3, "little")

    padding_bits = len(bitstream) * 8 - bit_position
    if padding_bits:
        used_bits = bit_position % 8
        padding_mask = 0xFF & ~((1 << used_bits) - 1)
        if bitstream[-1] & padding_mask:
            raise ValueError("nonzero block-delta padding")
    return bytes(decoded)


def pcm24_to_pcm32(pcm24: bytes) -> bytes:
    """Left-align signed PCM24 in PCM32, preserving normalized amplitude."""
    if len(pcm24) % 3:
        raise ValueError("PCM24 byte count is not sample aligned")
    pcm32 = bytearray((len(pcm24) // 3) * 4)
    for index in range(0, len(pcm24), 3):
        wire = int.from_bytes(pcm24[index:index + 3], "little")
        sample = wire - (1 << 24) if wire & (1 << 23) else wire
        # Keep all 24 valid bits in the high bits of the 32-bit WAV sample.
        # Sign extension alone would lower playback amplitude by 256 (~48 dB).
        struct.pack_into("<i", pcm32, (index // 3) * 4, sample << 8)
    return bytes(pcm32)


class PacketStreamParser:
    def __init__(self) -> None:
        self.buffer = bytearray()
        self.crc_errors = 0

    def reset(self) -> None:
        self.buffer.clear()

    @staticmethod
    def _header_valid(packet_type: int, channels: int, sample_rate: int,
                      frames: int, payload_bytes: int) -> bool:
        if packet_type not in PACKET_TYPES or channels != 1:
            return False
        if sample_rate <= 0 or sample_rate > 192000:
            return False
        if payload_bytes > MAX_PAYLOAD_BYTES:
            return False
        if packet_type in (PACKET_START, PACKET_END):
            return frames == 0 and payload_bytes == 0
        return 0 < frames <= MAX_FRAMES_PER_PACKET and payload_bytes > 0

    def feed(self, data: bytes) -> list[bytes]:
        self.buffer.extend(data)
        packets: list[bytes] = []
        while True:
            magic_index = self.buffer.find(MAGIC)
            if magic_index < 0:
                if self.buffer[-1:] == MAGIC[:1]:
                    del self.buffer[:-1]
                else:
                    self.buffer.clear()
                break
            if magic_index:
                del self.buffer[:magic_index]
            if len(self.buffer) < HEADER.size:
                break

            header = HEADER.unpack_from(self.buffer)
            _, packet_type, channels, sample_rate, frames, _, payload_bytes = header
            if not self._header_valid(packet_type, channels, sample_rate,
                                      frames, payload_bytes):
                del self.buffer[0]
                continue
            packet_length = HEADER.size + payload_bytes + CRC_BYTES
            if len(self.buffer) < packet_length:
                break
            candidate = self.buffer[:packet_length]
            expected_crc = CRC.unpack_from(candidate, packet_length - CRC_BYTES)[0]
            actual_crc = crc16_ccitt(memoryview(candidate)[:-CRC_BYTES])
            if actual_crc != expected_crc:
                self.crc_errors += 1
                del self.buffer[0]
                continue
            packets.append(bytes(candidate))
            del self.buffer[:packet_length]
        return packets


class Capture:
    def __init__(self, output: Path) -> None:
        self.output = output
        self.recording_index = 0
        self.segments_saved = 0
        self.segment_active = False
        self.wav_file: Optional[wave.Wave_write] = None
        self.output_file = None
        self.current_output: Optional[Path] = None
        self.audio_bytes = 0
        self.last_flush = time.monotonic()
        self.sample_rate: Optional[int] = None
        self.channels: Optional[int] = None
        self.frames_per_packet: Optional[int] = None
        self.expected_sequence: Optional[int] = None
        self.packets = 0
        self.lost_packets = 0
        self.disconnects = 0

    def start_segment(self, sample_rate: int, channels: int) -> None:
        if self.segment_active:
            self.save_pending_segment()
        self.sample_rate = sample_rate
        self.channels = channels
        self.frames_per_packet = None
        self.expected_sequence = None
        self.packets = 0
        self.lost_packets = 0
        self.audio_bytes = 0
        self.segment_active = True

    def handle_packet(self, packet: bytes) -> None:
        if len(packet) < HEADER.size + CRC_BYTES:
            return
        magic, packet_type, channels, sample_rate, frames, sequence, payload_bytes = (
            HEADER.unpack_from(packet)
        )
        if magic != MAGIC or payload_bytes != len(packet) - HEADER.size - CRC_BYTES:
            return
        expected_crc = CRC.unpack_from(packet, len(packet) - CRC_BYTES)[0]
        if crc16_ccitt(packet[:-CRC_BYTES]) != expected_crc:
            return

        if packet_type == PACKET_START:
            self.start_segment(sample_rate, channels)
            self.expected_sequence = sequence
            print("Recording segment started")
            return
        if packet_type == PACKET_END:
            if self.segment_active and self.frames_per_packet:
                self._fill_gap(sequence, self.frames_per_packet, channels)
            self.save_pending_segment()
            return
        if packet_type not in DATA_TYPES:
            return

        payload = packet[HEADER.size:-CRC_BYTES]
        try:
            if packet_type == PACKET_DATA_HUFFMAN:
                pcm24 = decode_huffman_pcm24(payload, frames)
            elif packet_type == PACKET_DATA_HUFFMAN_LEGACY:
                pcm24 = decode_legacy_huffman_pcm24(payload, frames)
            elif packet_type == PACKET_DATA_BLOCK_DELTA:
                pcm24 = decode_block_delta_pcm24(payload, frames)
            else:
                if payload_bytes != frames * channels * 3:
                    raise ValueError("unexpected raw PCM24 payload length")
                pcm24 = payload
            pcm32 = pcm24_to_pcm32(pcm24)
        except ValueError as exc:
            print(f"Ignoring malformed audio packet: {exc}", file=sys.stderr)
            return

        if not self.segment_active:
            self.start_segment(sample_rate, channels)
        if (sample_rate, channels) != (self.sample_rate, self.channels):
            print("Ignoring packet with changed audio format", file=sys.stderr)
            return
        if self.frames_per_packet is None:
            self.frames_per_packet = frames
            print(f"Audio stream: {sample_rate} Hz, {channels} channel, "
                  f"32-bit WAV, {frames} frames/packet")
        elif frames != self.frames_per_packet:
            print("Ignoring packet with changed frame count", file=sys.stderr)
            return

        if self.expected_sequence is None:
            self.expected_sequence = sequence
        if sequence < self.expected_sequence:
            return
        if sequence > self.expected_sequence:
            if not self._fill_gap(sequence, frames, channels):
                return
        self._write_audio(pcm32)
        self.expected_sequence = sequence + 1
        self.packets += 1

    def _fill_gap(self, sequence: int, frames: int, channels: int) -> bool:
        if self.expected_sequence is None or sequence <= self.expected_sequence:
            return True
        missing = sequence - self.expected_sequence
        # Bound silence insertion even though CRC protects the packet header.
        if missing * frames > self.sample_rate * 60:
            print("Ignoring implausible sequence gap (>60 seconds)", file=sys.stderr)
            return False
        remaining = missing * frames * channels * 4
        silence = bytes(65536)
        while remaining:
            chunk = min(remaining, len(silence))
            self._write_audio(silence[:chunk])
            remaining -= chunk
        self.lost_packets += missing
        print(f"Packet gap: expected {self.expected_sequence}, received {sequence} "
              f"({missing} missing)", file=sys.stderr)
        self.expected_sequence = sequence
        return True

    def _write_audio(self, data: bytes) -> None:
        if self.wav_file is None:
            self.current_output = self._next_output()
            self.current_output.parent.mkdir(parents=True, exist_ok=True)
            self.output_file = self.current_output.open("xb")
            self.wav_file = wave.open(self.output_file, "wb")
            self.wav_file.setnchannels(self.channels)
            self.wav_file.setsampwidth(4)
            self.wav_file.setframerate(self.sample_rate)
        # Patch WAV length after each block, flush once per second. Memory is
        # bounded independently of recording length.
        self.wav_file.writeframes(data)
        self.audio_bytes += len(data)
        now = time.monotonic()
        if now - self.last_flush >= 1.0:
            self.output_file.flush()
            self.last_flush = now

    def _next_output(self) -> Path:
        existing_indices: list[int] = []
        if self.output.exists():
            existing_indices.append(1)

        numbered_name = re.compile(
            rf"^{re.escape(self.output.stem)}_(\d{{3,}})"
            rf"{re.escape(self.output.suffix)}$"
        )
        parent = self.output.parent
        if parent.exists():
            for path in parent.iterdir():
                match = numbered_name.fullmatch(path.name)
                if match:
                    existing_indices.append(int(match.group(1)))

        index = max(existing_indices, default=0) + 1
        candidate = self.output if index == 1 else self.output.with_name(
            f"{self.output.stem}_{index:03d}{self.output.suffix}"
        )
        self.recording_index = index
        return candidate

    def save_pending_segment(self) -> None:
        if self.wav_file is not None:
            try:
                self.wav_file.close()
            finally:
                self.wav_file = None
                self.output_file.close()
                self.output_file = None
            seconds = self.audio_bytes / (self.sample_rate * self.channels * 4)
            print(f"Saved {self.current_output}: {seconds:.2f}s, {self.packets} "
                  f"packets, {self.lost_packets} lost packets")
            self.segments_saved += 1
        elif self.segment_active:
            print("Recording segment ended with no audio")
        self.audio_bytes = 0
        self.segment_active = False
        self.expected_sequence = None
        self.frames_per_packet = None
        self.packets = 0
        self.lost_packets = 0

    def mark_disconnected(self) -> None:
        self.disconnects += 1
        if self.segment_active or self.wav_file is not None:
            self.save_pending_segment()
        else:
            self.expected_sequence = None
            self.frames_per_packet = None


def find_bluetooth_com_port(address: str) -> str:
    if list_ports is None:
        raise RuntimeError("pyserial is required: python -m pip install pyserial")

    address_token = address.replace(":", "").replace("-", "").upper()
    bluetooth_ports = []
    address_matches = []
    for port in list_ports.comports():
        fields = [
            getattr(port, "device", None),
            getattr(port, "name", None),
            getattr(port, "description", None),
            getattr(port, "hwid", None),
            getattr(port, "pnp_device_id", None),
            getattr(port, "manufacturer", None),
            getattr(port, "product", None),
            getattr(port, "serial_number", None),
        ]
        details = " ".join(str(field) for field in fields if field)
        details_upper = details.upper()
        if "BTHENUM" not in details_upper and "BLUETOOTH" not in details_upper:
            continue
        bluetooth_ports.append((port.device, port.description, port.hwid))
        if address_token in re.sub(r"[^0-9A-F]", "", details_upper):
            address_matches.append(port.device)

    if len(address_matches) == 1:
        return address_matches[0]
    if len(address_matches) > 1:
        matches = ", ".join(address_matches)
        raise RuntimeError(
            f"multiple Bluetooth COM ports match {address}: {matches}; "
            "pass --port explicitly"
        )

    available = "; ".join(
        f"{device}: {description} [{hwid}]"
        for device, description, hwid in bluetooth_ports
    ) or "none"
    raise RuntimeError(
        f"no Windows Bluetooth COM port matches {address}; available: "
        f"{available}. Pair the device again or pass --port explicitly"
    )


class SerialReader:
    """The only COM reader. Decoding, printing and disk I/O stay off this thread."""
    QUEUE_CHUNKS = 256
    READ_BYTES = 4096

    def __init__(self, port) -> None:
        self.port = port
        self.chunks: queue.Queue = queue.Queue(self.QUEUE_CHUNKS)
        self.stop_event = threading.Event()
        self.done = threading.Event()
        self.lock = threading.Lock()
        self.raw_bytes = self.dropped_bytes = self.high_water = 0
        self.max_read_gap = 0.0
        self.generation = 0
        self.error: Optional[Exception] = None
        self.error_time = ""
        self.thread = threading.Thread(target=self._run, name="SPP COM reader",
                                       daemon=True)

    def start(self) -> None:
        self.thread.start()

    def _run(self) -> None:
        last = time.monotonic()
        try:
            while not self.stop_event.is_set():
                data = self.port.read(self.READ_BYTES)
                now = time.monotonic()
                with self.lock:
                    self.max_read_gap = max(self.max_read_gap, now - last)
                    self.raw_bytes += len(data)
                last = now
                if not data:
                    continue  # A read timeout is not a Bluetooth disconnect.
                try:
                    self.chunks.put_nowait((self.generation, data))
                    with self.lock:
                        self.high_water = max(self.high_water, self.chunks.qsize())
                except queue.Full:
                    # Keep draining COM; report loss and force parser resync at
                    # the next accepted chunk, without joining separated bytes.
                    self.generation += 1
                    with self.lock:
                        self.dropped_bytes += len(data)
        except Exception as exc:
            if not self.stop_event.is_set():
                self.error = exc
                self.error_time = datetime.now().astimezone().isoformat(timespec="milliseconds")
        finally:
            self.done.set()

    def snapshot(self) -> tuple[int, str]:
        with self.lock:
            return self.raw_bytes, (
                f"rx_queue={self.chunks.qsize()}/{self.QUEUE_CHUNKS} "
                f"high={self.high_water} dropped_bytes={self.dropped_bytes} "
                f"max_read_gap={self.max_read_gap * 1000:.0f}ms"
            )

    def stop(self) -> None:
        self.stop_event.set()
        cancel = getattr(self.port, "cancel_read", None)
        if cancel:
            try:
                cancel()
            except OSError:
                pass
        self.thread.join(timeout=2.0)
        if self.thread.is_alive():
            raise RuntimeError("COM reader did not stop; refusing an overlapping reconnect")


def consume_session(reader: SerialReader, capture: Capture, interval: float) -> None:
    parser = PacketStreamParser()
    rate_stats = ReceiveRateStats(interval)
    generation = 0
    previous_bytes = 0

    def consume(item) -> list[bytes]:
        nonlocal generation
        chunk_generation, data = item
        if chunk_generation != generation:
            parser.reset()
            generation = chunk_generation
            print("Receiver queue overflow: resynchronizing packet stream", file=sys.stderr)
        packets = parser.feed(data)
        for packet in packets:
            capture.handle_packet(packet)
        return packets

    reader.start()
    drain_pending = False
    try:
        while not reader.done.is_set() or not reader.chunks.empty():
            try:
                packets = consume(reader.chunks.get(timeout=0.1))
            except queue.Empty:
                packets = []
            raw_bytes, status = reader.snapshot()
            status = f"{status} crc_errors={parser.crc_errors}"
            rate_stats.update(raw_bytes - previous_bytes, packets, status)
            previous_bytes = raw_bytes
    except KeyboardInterrupt:
        drain_pending = True
        raise
    finally:
        reader.stop()
        # A disk/decoder exception must not re-enter the failed consumer.
        if drain_pending:
            while not reader.chunks.empty():
                consume(reader.chunks.get_nowait())
        total, status = reader.snapshot()
        print(f"Bluetooth session: received={total} B {status} "
              f"crc_errors={parser.crc_errors}")
    if reader.error is not None:
        error = reader.error
        print(f"[{reader.error_time}] COM read failed: {type(error).__name__}: "
              f"{error!r}; errno={getattr(error, 'errno', None)} "
              f"winerror={getattr(error, 'winerror', None)}", file=sys.stderr)


def receive(args: argparse.Namespace, capture: Capture) -> None:
    if serial is None:
        raise RuntimeError("pyserial is required: python -m pip install pyserial")

    reconnects = 0
    while True:
        port = None
        try:
            # Discovery/open failures are retriable; WAV/disk errors are not
            # mislabeled as Bluetooth failures.
            try:
                port_name = args.port or find_bluetooth_com_port(args.address)
                print(f"Opening Windows Bluetooth COM port {port_name} "
                      f"for {args.address}...")
                port = serial.Serial(port=port_name, baudrate=115200, timeout=0.05)
            except (RuntimeError, serial.SerialException, OSError) as exc:
                stamp = datetime.now().astimezone().isoformat(timespec="milliseconds")
                print(f"[{stamp}] COM discovery/open failed: "
                      f"{type(exc).__name__}: {exc!r}", file=sys.stderr)
            if port is not None:
                print("Bluetooth COM port opened; waiting for START packet.")
                consume_session(SerialReader(port), capture, args.stats_interval)
        finally:
            if port is not None:
                try:
                    port.close()
                except (serial.SerialException, OSError):
                    pass
                capture.mark_disconnected()

        reconnects += 1
        print(f"SPP reopen attempt {reconnects} in "
              f"{args.reconnect_delay:.1f} seconds", file=sys.stderr)
        time.sleep(args.reconnect_delay)


def bluetooth_address(value: str) -> str:
    normalized = value.strip().replace("-", ":").upper()
    if not re.fullmatch(r"(?:[0-9A-F]{2}:){5}[0-9A-F]{2}", normalized):
        raise argparse.ArgumentTypeError(
            "Bluetooth address must look like 24:6F:28:12:34:56"
        )
    return normalized


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--port",
        help="Windows Bluetooth virtual COM port; omitted means detect by MAC",
    )
    parser.add_argument(
        "--address",
        type=bluetooth_address,
        default=DEFAULT_BLUETOOTH_ADDRESS,
        help="ESP32 Bluetooth MAC used for automatic COM detection",
    )
    parser.add_argument(
        "-o", "--output", type=Path, default=Path("spp_capture.wav")
    )
    parser.add_argument("--reconnect-delay", type=float, default=1.0)
    parser.add_argument(
        "--stats-interval",
        type=float,
        default=1.0,
        help="seconds between Bluetooth receive-rate reports (default: 1.0)",
    )
    args = parser.parse_args()
    if not math.isfinite(args.stats_interval) or args.stats_interval <= 0:
        parser.error("--stats-interval must be finite and greater than zero")
    if not math.isfinite(args.reconnect_delay) or args.reconnect_delay <= 0:
        parser.error("--reconnect-delay must be finite and greater than zero")
    return args


def main() -> int:
    args = parse_args()
    capture = Capture(args.output)
    try:
        receive(args, capture)
    except KeyboardInterrupt:
        print("Stopping capture...")
    except Exception as exc:
        print(f"Error: {exc}", file=sys.stderr)
        return 1
    finally:
        if capture.segment_active or capture.wav_file is not None:
            capture.save_pending_segment()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
