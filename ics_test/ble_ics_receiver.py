#!/usr/bin/env python3
"""Receive the ICS-43434 BLE PCM stream and save it as a WAV file.

Install once with:  python -m pip install bleak
Run until Ctrl+C, or add --max-seconds N for a bounded recording.
"""

from __future__ import annotations

import argparse
import asyncio
import heapq
import struct
import sys
import time
import wave
from pathlib import Path
from typing import Optional

from bleak import BleakClient, BleakScanner


SERVICE_UUID = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
AUDIO_UUID = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"
MAGIC = b"\xA5\x5A"
PACKET_DATA = 1
PACKET_START = 2
PACKET_END = 3
PACKET_DATA_HUFFMAN = 0x81
MAX_FRAMES_PER_PACKET = 76
HEADER = struct.Struct("<2sBBHHIH")


def _huffman_frequency(symbol: int) -> int:
    # Mirror the fixed frequency model and node-order tie-break used by firmware.
    distance = min(symbol, 255 - symbol)
    radius = 1 + distance
    return 1 + 65535 // (radius * radius)


def _build_huffman_codes() -> dict[int, tuple[int, int]]:
    heap = [(_huffman_frequency(symbol), symbol, symbol, None, None)
            for symbol in range(256)]
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


def decode_huffman_pcm24(payload: bytes, frames: int) -> bytes:
    low_byte_count = frames * 2
    bitstream = payload[low_byte_count:]
    if len(payload) <= low_byte_count or len(bitstream) >= frames:
        raise ValueError("invalid Huffman payload size")

    decode_table = {(length, code): symbol
                    for symbol, (code, length) in HUFFMAN_CODES.items()}
    max_code_length = max(length for _, length in HUFFMAN_CODES.values())
    bit_position = 0
    decoded = bytearray(frames * 3)
    for frame in range(frames):
        code = 0
        high_byte = None
        for length in range(1, max_code_length + 1):
            if bit_position >= len(bitstream) * 8:
                raise ValueError("truncated Huffman bitstream")
            bit = (bitstream[bit_position // 8] >> (7 - bit_position % 8)) & 1
            bit_position += 1
            code = (code << 1) | bit
            high_byte = decode_table.get((length, code))
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
    if padding_bits and (bitstream[-1] & ((1 << padding_bits) - 1)):
        raise ValueError("nonzero Huffman padding")
    return bytes(decoded)


class Capture:
    def __init__(self, output: Path) -> None:
        self.output = output
        self.recording_index = 0
        self.segments_saved = 0
        self.segment_active = False
        self.audio = bytearray()
        self.sample_rate: Optional[int] = None
        self.channels: Optional[int] = None
        self.sample_width: Optional[int] = None
        self.expected_sequence: Optional[int] = None
        self.packet_payload_size: Optional[int] = None
        self.packets = 0
        self.lost_packets = 0
        self.last_packet_time = time.monotonic()
        self.disconnects = 0

    def on_notification(self, _sender: object, data: bytearray) -> None:
        if len(data) < HEADER.size:
            return
        magic, packet_type, channels, sample_rate, frames, sequence, payload_bytes = (
            HEADER.unpack_from(data)
        )
        if magic != MAGIC or packet_type not in (
            PACKET_DATA, PACKET_DATA_HUFFMAN, PACKET_START, PACKET_END
        ):
            return
        if channels != 1 or payload_bytes != len(data) - HEADER.size:
            return

        if packet_type == PACKET_START:
            if payload_bytes != 0:
                return
            self.start_segment(sample_rate, channels)
            print("Recording segment started")
            return

        if packet_type == PACKET_END:
            if payload_bytes != 0:
                return
            self.save_pending_segment()
            return

        if frames == 0 or frames > MAX_FRAMES_PER_PACKET:
            return
        try:
            if packet_type == PACKET_DATA_HUFFMAN:
                if channels != 1:
                    return
                audio_payload = decode_huffman_pcm24(
                    bytes(data[HEADER.size:]), frames
                )
                sample_width = 3
            else:
                if payload_bytes % (frames * channels) != 0:
                    return
                sample_width = payload_bytes // (frames * channels)
                if sample_width not in (2, 3):
                    return
                audio_payload = bytes(data[HEADER.size:])
        except ValueError as exc:
            print(f"Ignoring malformed audio packet: {exc}", file=sys.stderr)
            return
        if not self.segment_active:
            # Be tolerant of a START packet lost during a reconnect; the first
            # data packet can still begin a segment.
            self.start_segment(sample_rate, channels)
        if (sample_rate, channels) != (self.sample_rate, self.channels):
            print("Ignoring packet with changed audio format", file=sys.stderr)
            return
        if self.sample_width is None:
            self.sample_width = sample_width
            print(
                f"Audio stream: {sample_rate} Hz, {channels} channel, "
                f"{sample_width * 8}-bit, {frames} frames/packet"
            )
        elif sample_width != self.sample_width:
            print("Ignoring packet with changed sample width", file=sys.stderr)
            return

        if self.expected_sequence is None:
            self.expected_sequence = sequence
        if sequence < self.expected_sequence:
            return
        if self.packet_payload_size is None:
            self.packet_payload_size = len(audio_payload)
        if sequence > self.expected_sequence:
            missing = sequence - self.expected_sequence
            print(
                f"Packet gap: expected {self.expected_sequence}, received {sequence} "
                f"({missing} missing)",
                file=sys.stderr,
            )
            self.audio.extend(b"\x00" * (missing * self.packet_payload_size))
            self.lost_packets += missing
        self.audio.extend(audio_payload)
        self.expected_sequence = sequence + 1
        self.packets += 1
        self.last_packet_time = time.monotonic()

    def start_segment(self, sample_rate: int, channels: int) -> None:
        self.sample_rate = sample_rate
        self.channels = channels
        self.sample_width = None
        self.audio.clear()
        self.expected_sequence = None
        self.packet_payload_size = None
        self.packets = 0
        self.lost_packets = 0
        self.segment_active = True
        self.last_packet_time = time.monotonic()

    def save_pending_segment(self) -> None:
        if not self.audio or not self.sample_rate or not self.channels:
            self.segment_active = False
            self.audio.clear()
            self.expected_sequence = None
            self.packet_payload_size = None
            self.sample_width = None
            self.packets = 0
            self.lost_packets = 0
            print("Recording segment ended with no audio")
            return

        self.recording_index += 1
        output = self.output if self.recording_index == 1 else self.output.with_name(
            f"{self.output.stem}_{self.recording_index:03d}{self.output.suffix}"
        )
        self.save(output)
        self.segments_saved += 1
        self.audio.clear()
        self.segment_active = False
        self.expected_sequence = None
        self.packet_payload_size = None
        self.sample_width = None
        self.packets = 0
        self.lost_packets = 0

    def mark_disconnected(self) -> None:
        """Close the current segment without inventing samples for the gap."""
        self.disconnects += 1
        if self.segment_active or self.audio:
            self.save_pending_segment()
        else:
            self.expected_sequence = None
            self.packet_payload_size = None

    @property
    def seconds(self) -> float:
        if not self.sample_rate or not self.channels:
            return 0.0
        if not self.sample_width:
            return 0.0
        return len(self.audio) / (self.sample_rate * self.channels * self.sample_width)

    def save(self, output: Optional[Path] = None) -> None:
        if not self.audio or not self.sample_rate or not self.channels or not self.sample_width:
            raise RuntimeError("No valid BLE audio packets received")
        output = output or self.output
        output.parent.mkdir(parents=True, exist_ok=True)
        with wave.open(str(output), "wb") as wav_file:
            wav_file.setnchannels(self.channels)
            wav_file.setsampwidth(self.sample_width)
            wav_file.setframerate(self.sample_rate)
            wav_file.writeframes(self.audio)
        print(
            f"Saved {output}: {self.seconds:.2f}s, {self.packets} packets, "
            f"{self.lost_packets} lost packets"
        )


async def find_device(name: str, address: Optional[str]):
    if address:
        return address

    def matches(device, advertisement_data):
        advertised_name = (advertisement_data.local_name or device.name or "").strip()
        services = [uuid.lower() for uuid in (advertisement_data.service_uuids or [])]
        return advertised_name.lower() == name.strip().lower() or SERVICE_UUID.lower() in services

    device = await BleakScanner.find_device_by_filter(matches, timeout=10.0)
    if device is None:
        raise RuntimeError(f"Could not find {name!r}; use --address or check advertising")
    return device


async def receive(args: argparse.Namespace, capture: Capture) -> None:
    started = time.monotonic()
    reconnect_count = 0
    while True:
        if args.max_seconds > 0:
            remaining = args.max_seconds - (time.monotonic() - started)
            if remaining <= 0:
                break

        connected = False
        disconnected = asyncio.Event()

        def on_disconnect(_client: BleakClient) -> None:
            disconnected.set()

        try:
            device = await find_device(args.name, args.address)
            label = getattr(device, "name", None) or getattr(device, "address", device)
            print(f"Connecting to {label}...")
            async with BleakClient(device, disconnected_callback=on_disconnect) as client:
                await client.start_notify(AUDIO_UUID, capture.on_notification)
                connected = True
                print("Connected. Recording; waiting for START packet.")
                while not disconnected.is_set():
                    sleep_for = 0.1
                    if args.max_seconds > 0:
                        sleep_for = min(
                            sleep_for,
                            max(0.0, args.max_seconds - (time.monotonic() - started)),
                        )
                    if sleep_for == 0:
                        break
                    await asyncio.sleep(sleep_for)
                if not disconnected.is_set():
                    try:
                        await client.stop_notify(AUDIO_UUID)
                    except Exception:
                        pass
        except asyncio.CancelledError:
            raise
        except Exception as exc:
            print(f"BLE connection error: {exc}", file=sys.stderr)
        finally:
            if connected:
                if args.max_seconds > 0 and time.monotonic() - started >= args.max_seconds:
                    capture.save_pending_segment()
                else:
                    capture.mark_disconnected()

        if args.max_seconds > 0 and time.monotonic() - started >= args.max_seconds:
            break
        reconnect_count += 1
        print(
            f"BLE disconnected; reconnect attempt {reconnect_count} in 1 second",
            file=sys.stderr,
        )
        await asyncio.sleep(1.0)

    if capture.segment_active or capture.audio:
        capture.save_pending_segment()
    print(f"BLE reconnects: {reconnect_count}, disconnects: {capture.disconnects}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("-o", "--output", type=Path, default=Path("ics_capture.wav"))
    parser.add_argument("--name", default="ESP32S3-ICS")
    parser.add_argument("--address", help="BLE address/identifier; skips scanning")
    parser.add_argument("--max-seconds", type=float, default=0.0,
                        help="stop after N seconds; 0 means Ctrl+C")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    capture = Capture(args.output)
    failed = False
    try:
        asyncio.run(receive(args, capture))
    except KeyboardInterrupt:
        print("Stopping capture...")
    except Exception as exc:
        print(f"Error: {exc}", file=sys.stderr)
        failed = True
    if capture.audio:
        try:
            capture.save_pending_segment()
        except Exception as exc:
            print(f"Could not save WAV: {exc}", file=sys.stderr)
            return 1
    if capture.segments_saved > 0:
        return 1 if failed else 0
    print("No audio received", file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
