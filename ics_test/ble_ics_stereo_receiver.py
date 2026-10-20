#!/usr/bin/env python3
"""Receive the dual-microphone BLE stream and save stereo PCM WAV files.

Install Bleak with: python -m pip install bleak
Run until Ctrl+C, or pass --max-seconds N for a bounded capture.
"""

from __future__ import annotations

import argparse
import asyncio
import struct
import sys
import wave
from pathlib import Path
from typing import Optional


SERVICE_UUID = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
AUDIO_UUID = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"
MAGIC = b"\xA5\x5A"
PACKET_DATA = 1
PACKET_START = 2
PACKET_END = 3
CHANNELS = 2
SAMPLE_WIDTH = 2
HEADER = struct.Struct("<2sBBHHIH")


class Capture:
    def __init__(self, output: Path) -> None:
        self.output = output
        self.recording_index = 0
        self.segments_saved = 0
        self.segment_active = False
        self.audio = bytearray()
        self.sample_rate: Optional[int] = None
        self.expected_sequence: Optional[int] = None
        self.packet_payload_size: Optional[int] = None
        self.packets = 0
        self.lost_packets = 0

    def on_notification(self, _sender: object, data: bytearray) -> None:
        if len(data) < HEADER.size:
            return

        magic, packet_type, channels, sample_rate, frames, sequence, payload_bytes = (
            HEADER.unpack_from(data)
        )
        if magic != MAGIC or packet_type not in (PACKET_DATA, PACKET_START, PACKET_END):
            return
        if channels != CHANNELS or payload_bytes != len(data) - HEADER.size:
            return

        if packet_type == PACKET_START:
            if frames != 0 or payload_bytes != 0 or sample_rate <= 0:
                return
            self.start_segment(sample_rate)
            print(f"Stereo recording started: {sample_rate} Hz")
            return

        if packet_type == PACKET_END:
            if frames != 0 or payload_bytes != 0:
                return
            self.save_pending_segment()
            return

        expected_bytes = frames * CHANNELS * SAMPLE_WIDTH
        if frames == 0 or payload_bytes != expected_bytes or sample_rate <= 0:
            return
        if not self.segment_active:
            self.start_segment(sample_rate)
        if sample_rate != self.sample_rate:
            print("Ignoring packet with changed sample rate", file=sys.stderr)
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

    def start_segment(self, sample_rate: int) -> None:
        self.sample_rate = sample_rate
        self.audio.clear()
        self.expected_sequence = None
        self.packet_payload_size = None
        self.packets = 0
        self.lost_packets = 0
        self.segment_active = True

    def save_pending_segment(self) -> None:
        if not self.audio or not self.sample_rate:
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

    def save(self, output: Path) -> None:
        if not self.audio or not self.sample_rate:
            raise RuntimeError("No valid stereo BLE audio packets received")
        output.parent.mkdir(parents=True, exist_ok=True)
        with wave.open(str(output), "wb") as wav_file:
            wav_file.setnchannels(CHANNELS)
            wav_file.setsampwidth(SAMPLE_WIDTH)
            wav_file.setframerate(self.sample_rate)
            wav_file.writeframes(self.audio)
        duration = len(self.audio) / (self.sample_rate * CHANNELS * SAMPLE_WIDTH)
        print(
            f"Saved {output}: {duration:.2f}s stereo, {self.packets} packets, "
            f"{self.lost_packets} lost packets"
        )


async def find_device(name: str, address: Optional[str]):
    from bleak import BleakScanner

    if address:
        return address

    def matches(device, advertisement_data):
        advertised_name = (advertisement_data.local_name or device.name or "").strip()
        services = [uuid.lower() for uuid in (advertisement_data.service_uuids or [])]
        return (
            advertised_name.lower() == name.strip().lower()
            or SERVICE_UUID.lower() in services
        )

    device = await BleakScanner.find_device_by_filter(matches, timeout=10.0)
    if device is None:
        raise RuntimeError(f"Could not find {name!r}; use --address or check advertising")
    return device


async def receive(args: argparse.Namespace, capture: Capture) -> None:
    from bleak import BleakClient

    device = await find_device(args.name, args.address)
    label = getattr(device, "name", None) or getattr(device, "address", device)
    print(f"Connecting to {label}...")
    disconnected = asyncio.Event()

    def on_disconnect(_client) -> None:
        disconnected.set()

    async with BleakClient(device, disconnected_callback=on_disconnect) as client:
        await client.start_notify(AUDIO_UUID, capture.on_notification)
        print("Connected. Stereo capture active; press Ctrl+C to stop.")
        try:
            if args.max_seconds > 0:
                try:
                    await asyncio.wait_for(disconnected.wait(), timeout=args.max_seconds)
                    raise ConnectionError("BLE peripheral disconnected")
                except asyncio.TimeoutError:
                    pass
            else:
                await disconnected.wait()
                raise ConnectionError("BLE peripheral disconnected")
        finally:
            try:
                await client.stop_notify(AUDIO_UUID)
            except Exception:
                pass
            if capture.audio:
                capture.save_pending_segment()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "-o", "--output", type=Path, default=Path("ics_capture_stereo.wav")
    )
    parser.add_argument("--name", default="ESP32S3-ICS")
    parser.add_argument("--address", help="BLE address/identifier; skips scanning")
    parser.add_argument(
        "--max-seconds", type=float, default=0.0,
        help="stop after N seconds; 0 means Ctrl+C",
    )
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
    print("No stereo audio received", file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
