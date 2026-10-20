#!/usr/bin/env python3
"""Receive the ICS-43434 BLE PCM stream and save it as a WAV file.

Install once with:  python -m pip install bleak
Run until Ctrl+C, or add --max-seconds N for a bounded recording.
"""

from __future__ import annotations

import argparse
import asyncio
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
HEADER = struct.Struct("<2sBBHHIH")


class Capture:
    def __init__(self, output: Path) -> None:
        self.output = output
        self.recording_index = 0
        self.segments_saved = 0
        self.segment_active = False
        self.audio = bytearray()
        self.sample_rate: Optional[int] = None
        self.channels: Optional[int] = None
        self.expected_sequence: Optional[int] = None
        self.packet_payload_size: Optional[int] = None
        self.packets = 0
        self.lost_packets = 0
        self.last_packet_time = time.monotonic()

    def on_notification(self, _sender: object, data: bytearray) -> None:
        if len(data) < HEADER.size:
            return
        magic, packet_type, channels, sample_rate, frames, sequence, payload_bytes = (
            HEADER.unpack_from(data)
        )
        if magic != MAGIC or packet_type not in (PACKET_DATA, PACKET_START, PACKET_END):
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

        if payload_bytes != frames * channels * 2 or frames == 0:
            return
        if not self.segment_active:
            # Be tolerant of a START packet lost during a reconnect; the first
            # data packet can still begin a segment.
            self.start_segment(sample_rate, channels)
        if self.sample_rate is None:
            self.sample_rate = sample_rate
            self.channels = channels
            print(f"Audio stream: {sample_rate} Hz, {channels} channel, {frames} frames/packet")
        if (sample_rate, channels) != (self.sample_rate, self.channels):
            print("Ignoring packet with changed audio format", file=sys.stderr)
            return

        if self.expected_sequence is None:
            self.expected_sequence = sequence
        if sequence < self.expected_sequence:
            return
        if self.packet_payload_size is None:
            self.packet_payload_size = payload_bytes
        if sequence > self.expected_sequence:
            missing = sequence - self.expected_sequence
            self.audio.extend(b"\x00" * (missing * self.packet_payload_size))
            self.lost_packets += missing
        self.audio.extend(data[HEADER.size:])
        self.expected_sequence = sequence + 1
        self.packets += 1
        self.last_packet_time = time.monotonic()

    def start_segment(self, sample_rate: int, channels: int) -> None:
        self.sample_rate = sample_rate
        self.channels = channels
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
        self.packets = 0
        self.lost_packets = 0

    @property
    def seconds(self) -> float:
        if not self.sample_rate or not self.channels:
            return 0.0
        return len(self.audio) / (self.sample_rate * self.channels * 2)

    def save(self, output: Optional[Path] = None) -> None:
        if not self.audio or not self.sample_rate or not self.channels:
            raise RuntimeError("No valid BLE audio packets received")
        output = output or self.output
        output.parent.mkdir(parents=True, exist_ok=True)
        with wave.open(str(output), "wb") as wav_file:
            wav_file.setnchannels(self.channels)
            wav_file.setsampwidth(2)
            wav_file.setframerate(self.sample_rate)
            wav_file.writeframes(self.audio)
        print(
            f"Saved {output}: {self.seconds:.2f}s, {self.packets} packets, "
            f"{self.lost_packets} lost packets"
        )


async def find_device(name: str, address: Optional[str], scan_timeout: float):
    if address:
        return address

    def matches(device, advertisement_data):
        advertised_name = (advertisement_data.local_name or device.name or "").strip()
        services = [uuid.lower() for uuid in (advertisement_data.service_uuids or [])]
        return advertised_name.lower() == name.strip().lower() or SERVICE_UUID.lower() in services

    device = await BleakScanner.find_device_by_filter(matches, timeout=scan_timeout)
    if device is None:
        raise RuntimeError(f"Could not find {name!r}; use --address or check advertising")
    return device


async def receive(args: argparse.Namespace, capture: Capture) -> None:
    device = await find_device(args.name, args.address, args.scan_timeout)
    print(f"Connecting to {getattr(device, 'name', None) or getattr(device, 'address', device)}...")
    disconnected = asyncio.Event()

    def on_disconnect(_client: BleakClient) -> None:
        disconnected.set()

    async with BleakClient(
        device, timeout=args.connect_timeout, disconnected_callback=on_disconnect
    ) as client:
        await client.start_notify(AUDIO_UUID, capture.on_notification)
        print("Connected. Recording; press Ctrl+C to stop.")
        try:
            if args.max_seconds > 0:
                try:
                    await asyncio.wait_for(disconnected.wait(), timeout=args.max_seconds)
                except asyncio.TimeoutError:
                    return
            else:
                await disconnected.wait()
            print("BLE connection lost while receiving audio", file=sys.stderr)
            raise ConnectionError("BLE peripheral disconnected")
        finally:
            try:
                await client.stop_notify(AUDIO_UUID)
            except Exception:
                pass
            capture.save_pending_segment()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("-o", "--output", type=Path, default=Path("ics_capture.wav"))
    parser.add_argument("--name", default="ESP32S3-ICS")
    parser.add_argument("--address", help="BLE address/identifier; skips scanning")
    parser.add_argument("--scan-timeout", type=float, default=30.0,
                        help="BLE scan timeout in seconds")
    parser.add_argument("--connect-timeout", type=float, default=30.0,
                        help="BLE connection/setup timeout in seconds")
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
