#include "packet_protocol.h"

namespace {
// 包同步字节，接收端用它快速识别协议帧边界。
constexpr uint8_t kMagic0 = 0xA5;
constexpr uint8_t kMagic1 = 0x5A;
}

namespace AudioProtocol {
// ATT MTU 包含 3 字节 ATT 操作开销；再扣除本项目 14 字节头部后换算帧数。
size_t framesForMtu(uint16_t mtu) {
  const size_t mtuPayload = mtu > 3 ? mtu - 3 : 0;
  const size_t usablePayload = mtuPayload > BLEConfig::HEADER_BYTES
                                   ? mtuPayload - BLEConfig::HEADER_BYTES
                                   : 0;
  size_t frames = usablePayload /
      (AudioConfig::CHANNELS * (AudioConfig::BITS_PER_SAMPLE / 8));
  if (frames > BLEConfig::MAX_PACKET_FRAMES) {
    frames = BLEConfig::MAX_PACKET_FRAMES;
  }
  return frames;
}

// 显式按小端序编码，保证协议不依赖主机 CPU 的字节序。
void putLe16(uint8_t *destination, uint16_t value) {
  destination[0] = static_cast<uint8_t>(value & 0xffu);
  destination[1] = static_cast<uint8_t>((value >> 8) & 0xffu);
}

// 32 位序号占据头部偏移 8..11。
void putLe32(uint8_t *destination, uint32_t value) {
  destination[0] = static_cast<uint8_t>(value & 0xffu);
  destination[1] = static_cast<uint8_t>((value >> 8) & 0xffu);
  destination[2] = static_cast<uint8_t>((value >> 16) & 0xffu);
  destination[3] = static_cast<uint8_t>((value >> 24) & 0xffu);
}

// START/END 的 14 字节头部包含格式和序号，帧数及负载长度均为零。
void buildControlPacket(Packet &packet, PacketType type, uint32_t sequence) {
  packet.length = BLEConfig::HEADER_BYTES;
  packet.bytes[0] = kMagic0;
  packet.bytes[1] = kMagic1;
  packet.bytes[2] = static_cast<uint8_t>(type);
  packet.bytes[3] = AudioConfig::CHANNELS;
  putLe16(packet.bytes + 4, static_cast<uint16_t>(AudioConfig::SAMPLE_RATE));
  putLe16(packet.bytes + 6, 0);
  putLe32(packet.bytes + 8, sequence);
  putLe16(packet.bytes + 12, 0);
}

// DATA 布局：A5 5A、类型、声道、采样率、帧数、序号、负载长度、PCM 负载。
void buildAudioPacket(Packet &packet, const int16_t *samples, size_t frames,
                      uint32_t sequence) {
  if (frames > BLEConfig::MAX_PACKET_FRAMES) {
    frames = BLEConfig::MAX_PACKET_FRAMES;
  }
  const size_t payloadBytes =
      frames * AudioConfig::CHANNELS * (AudioConfig::BITS_PER_SAMPLE / 8);
  packet.length = BLEConfig::HEADER_BYTES + payloadBytes;
  packet.bytes[0] = kMagic0;
  packet.bytes[1] = kMagic1;
  packet.bytes[2] = static_cast<uint8_t>(PacketType::DATA);
  packet.bytes[3] = AudioConfig::CHANNELS;
  putLe16(packet.bytes + 4, static_cast<uint16_t>(AudioConfig::SAMPLE_RATE));
  putLe16(packet.bytes + 6, static_cast<uint16_t>(frames));
  putLe32(packet.bytes + 8, sequence);
  putLe16(packet.bytes + 12, static_cast<uint16_t>(payloadBytes));

  for (size_t i = 0; i < frames; ++i) {
    const uint16_t wireSample = static_cast<uint16_t>(samples[i]);
    packet.bytes[BLEConfig::HEADER_BYTES + 2 * i] =
        static_cast<uint8_t>(wireSample & 0xffu);
    packet.bytes[BLEConfig::HEADER_BYTES + 2 * i + 1] =
        static_cast<uint8_t>((wireSample >> 8) & 0xffu);
  }
}
}  // namespace AudioProtocol
