#include "packet_protocol.h"

namespace IcsTestProtocol {
namespace {

constexpr uint8_t kMagic0 = 0xA5;
constexpr uint8_t kMagic1 = 0x5A;

void putLe16(uint8_t *dst, uint16_t value) {
  dst[0] = static_cast<uint8_t>(value & 0xffu);
  dst[1] = static_cast<uint8_t>((value >> 8) & 0xffu);
}

void putLe32(uint8_t *dst, uint32_t value) {
  dst[0] = static_cast<uint8_t>(value & 0xffu);
  dst[1] = static_cast<uint8_t>((value >> 8) & 0xffu);
  dst[2] = static_cast<uint8_t>((value >> 16) & 0xffu);
  dst[3] = static_cast<uint8_t>((value >> 24) & 0xffu);
}

int16_t clampPcm16(float value) {
  if (value > 32767.0f) value = 32767.0f;
  if (value < -32768.0f) value = -32768.0f;
  return static_cast<int16_t>(value);
}

void buildHeader(PacketType type, uint16_t frames, uint32_t sequence,
                 uint16_t payloadBytes, BlePacket &packet) {
  packet.bytes[0] = kMagic0;
  packet.bytes[1] = kMagic1;
  packet.bytes[2] = static_cast<uint8_t>(type);
  packet.bytes[3] = IcsTestConfig::AUDIO_CHANNELS;
  putLe16(packet.bytes + 4,
          static_cast<uint16_t>(IcsTestConfig::AUDIO_SAMPLE_RATE));
  putLe16(packet.bytes + 6, frames);
  putLe32(packet.bytes + 8, sequence);
  putLe16(packet.bytes + 12, payloadBytes);
}

}  // namespace

void buildControlPacket(PacketType type, uint32_t sequence, BlePacket &packet) {
  packet.length = IcsTestConfig::HEADER_BYTES;
  buildHeader(type, 0, sequence, 0, packet);
}

void buildAudioPacket(const float *samples, uint32_t &sequence,
                      BlePacket &packet) {
  packet.length = IcsTestConfig::HEADER_BYTES +
                  IcsTestConfig::MAX_PAYLOAD_BYTES;
  buildHeader(DATA, IcsTestConfig::MAX_PACKET_FRAMES, sequence++,
              IcsTestConfig::MAX_PAYLOAD_BYTES, packet);
  for (size_t i = 0; i < IcsTestConfig::MAX_PACKET_FRAMES; ++i) {
    const uint16_t sample = static_cast<uint16_t>(clampPcm16(samples[i]));
    packet.bytes[IcsTestConfig::HEADER_BYTES + 2 * i] =
        static_cast<uint8_t>(sample & 0xffu);
    packet.bytes[IcsTestConfig::HEADER_BYTES + 2 * i + 1] =
        static_cast<uint8_t>((sample >> 8) & 0xffu);
  }
}

}  // namespace IcsTestProtocol
