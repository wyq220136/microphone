#include "packet_protocol.h"
#if SINGLEMIC_ENABLE_HUFFMAN
#include "task_cooperation.h"
#endif
#include "reset_progress.h"

#include <string.h>

namespace {
// 包同步字节，接收端用它快速识别协议帧边界。
constexpr uint8_t kMagic0 = 0xA5;
constexpr uint8_t kMagic1 = 0x5A;

void appendCrc(AudioProtocol::Packet &packet, size_t bytesWithoutCrc) {
  ResetProgress::main(ResetProgress::Stage::CRC);
  const uint16_t crc =
      AudioProtocol::crc16Ccitt(packet.bytes, bytesWithoutCrc);
  AudioProtocol::putLe16(packet.bytes + bytesWithoutCrc, crc);
  packet.length = static_cast<uint16_t>(
      bytesWithoutCrc + PacketConfig::CRC_BYTES);
  ResetProgress::main(ResetProgress::Stage::ENQUEUE);
}

#if SINGLEMIC_ENABLE_HUFFMAN
struct HuffmanCode {
  uint32_t bits = 0;
  uint8_t length = 0;
};

struct HuffmanNode {
  uint32_t frequency = 0;
  int16_t left = -1;
  int16_t right = -1;
  uint16_t symbol = 0;
};

// Stop encoding when it cannot fit; raw PCM24 is the bounded fallback.
constexpr size_t kMaxHuffmanBytes = PacketConfig::MAX_PAYLOAD_BYTES;

// The encoder is called by the single application/producer task. Keep its
// scratch storage in one explicitly owned object instead of placing a large
// byte array on that task's stack for every packet. This object is not shared
// with the Bluetooth owner task; the completed Packet is copied into the FIFO
// before the call returns.
struct PacketBuildWorkspace {
  uint8_t compressedPcm24[kMaxHuffmanBytes] = {};
};

PacketBuildWorkspace packetBuildWorkspace;
HuffmanNode huffmanNodes[511] = {};
bool huffmanNodeActive[511] = {};
uint8_t huffmanLengths[256] = {};
HuffmanCode huffmanCodes[256] = {};
bool huffmanCodesReady = false;

uint32_t huffmanFrequency(uint16_t symbol) {
  const uint16_t distance = symbol < 128 ? symbol : 255 - symbol;
  const uint32_t radius = 1u + distance;
  return 1u + 65535u / (radius * radius);
}

void assignHuffmanLengths(const HuffmanNode *nodes, int16_t nodeIndex,
                          uint8_t depth, uint8_t *lengths) {
  const HuffmanNode &node = nodes[nodeIndex];
  if (node.left < 0 && node.right < 0) {
    lengths[node.symbol] = depth == 0 ? 1 : depth;
    return;
  }
  assignHuffmanLengths(nodes, node.left, static_cast<uint8_t>(depth + 1),
                       lengths);
  assignHuffmanLengths(nodes, node.right, static_cast<uint8_t>(depth + 1),
                       lengths);
}

void buildHuffmanCodes() {
  if (huffmanCodesReady) return;
  ResetProgress::main(ResetProgress::Stage::HUFFMAN_INIT);
  TaskCooperation::Progress progress;

  for (uint16_t symbol = 0; symbol < 256; ++symbol) {
    huffmanNodes[symbol].frequency = huffmanFrequency(symbol);
    huffmanNodes[symbol].symbol = symbol;
    huffmanNodeActive[symbol] = true;
  }

  int16_t nextNode = 256;
  for (uint16_t merge = 0; merge < 255; ++merge) {
    int16_t first = -1;
    int16_t second = -1;
    for (int16_t candidate = 0; candidate < nextNode; ++candidate) {
      if (!huffmanNodeActive[candidate]) continue;
      if (first < 0 ||
          huffmanNodes[candidate].frequency < huffmanNodes[first].frequency ||
          (huffmanNodes[candidate].frequency == huffmanNodes[first].frequency &&
           candidate < first)) {
        second = first;
        first = candidate;
      } else if (second < 0 ||
                 huffmanNodes[candidate].frequency <
                     huffmanNodes[second].frequency ||
                 (huffmanNodes[candidate].frequency ==
                      huffmanNodes[second].frequency &&
                  candidate < second)) {
        second = candidate;
      }
    }

    huffmanNodes[nextNode].frequency =
        huffmanNodes[first].frequency + huffmanNodes[second].frequency;
    huffmanNodes[nextNode].left = first;
    huffmanNodes[nextNode].right = second;
    huffmanNodeActive[first] = false;
    huffmanNodeActive[second] = false;
    huffmanNodeActive[nextNode] = true;
    ++nextNode;
    progress.checkpoint();
  }

  assignHuffmanLengths(huffmanNodes, static_cast<int16_t>(nextNode - 1), 0,
                       huffmanLengths);

  uint32_t code = 0;
  uint8_t previousLength = 0;
  for (uint8_t length = 1; length <= 32; ++length) {
    for (uint16_t symbol = 0; symbol < 256; ++symbol) {
      if (huffmanLengths[symbol] != length) continue;
      code <<= static_cast<uint8_t>(length - previousLength);
      huffmanCodes[symbol].bits = code;
      huffmanCodes[symbol].length = length;
      ++code;
      previousLength = length;
    }
    progress.checkpoint();
  }
  huffmanCodesReady = true;
}

bool encodeHuffmanPcm24(const int32_t *samples, size_t frames,
                            uint8_t *destination, size_t destinationCapacity,
                            size_t &encodedBytes) {
  buildHuffmanCodes();
  ResetProgress::main(ResetProgress::Stage::HUFFMAN_ENCODE);
  memset(destination, 0, destinationCapacity);
  size_t bitPosition = 0;
  for (size_t i = 0; i < frames; ++i) {
    // Encode all three little-endian PCM24 bytes as ordinary byte symbols.
    // No raw low-word prefix, predictor, delta or sample quantization.
    const uint32_t sample = static_cast<uint32_t>(samples[i]);
    for (unsigned byte = 0; byte < 3; ++byte) {
      const uint8_t symbol = static_cast<uint8_t>(sample >> (byte * 8));
      const HuffmanCode code = huffmanCodes[symbol];
      if (bitPosition + code.length > destinationCapacity * 8) return false;
      for (int bit = static_cast<int>(code.length) - 1; bit >= 0; --bit) {
        if ((code.bits & (1u << bit)) != 0) {
          destination[bitPosition >> 3] |=
              static_cast<uint8_t>(1u << (7u - (bitPosition & 7u)));
        }
        ++bitPosition;
      }
    }
  }
  encodedBytes = (bitPosition + 7) / 8;
  return encodedBytes < frames * AudioConfig::BYTES_PER_SAMPLE;
}
#endif  // SINGLEMIC_ENABLE_HUFFMAN

}

namespace AudioProtocol {
// ATT MTU 包含 3 字节 ATT 操作开销；再扣除本项目 14 字节头部后换算帧数。
size_t framesForMtu(uint16_t mtu) {
  const size_t mtuPayload = mtu > 3 ? mtu - 3 : 0;
  const size_t overhead =
      PacketConfig::HEADER_BYTES + PacketConfig::CRC_BYTES;
  const size_t usablePayload = mtuPayload > overhead
                                   ? mtuPayload - overhead
                                   : 0;
  size_t frames = usablePayload /
      (AudioConfig::CHANNELS * AudioConfig::BYTES_PER_SAMPLE);
  if (frames > PacketConfig::MAX_PACKET_FRAMES) {
    frames = PacketConfig::MAX_PACKET_FRAMES;
  }
  return frames;
}

// 显式按小端序编码，保证协议不依赖主机 CPU 的字节序。
void putLe16(uint8_t *destination, uint16_t value) {
  destination[0] = static_cast<uint8_t>(value & 0xffu);
  destination[1] = static_cast<uint8_t>((value >> 8) & 0xffu);
}

void putLe24(uint8_t *destination, int32_t value) {
  const uint32_t wireValue = static_cast<uint32_t>(value) & 0x00ffffffu;
  destination[0] = static_cast<uint8_t>(wireValue & 0xffu);
  destination[1] = static_cast<uint8_t>((wireValue >> 8) & 0xffu);
  destination[2] = static_cast<uint8_t>((wireValue >> 16) & 0xffu);
}

// 32 位序号占据头部偏移 8..11。
void putLe32(uint8_t *destination, uint32_t value) {
  destination[0] = static_cast<uint8_t>(value & 0xffu);
  destination[1] = static_cast<uint8_t>((value >> 8) & 0xffu);
  destination[2] = static_cast<uint8_t>((value >> 16) & 0xffu);
  destination[3] = static_cast<uint8_t>((value >> 24) & 0xffu);
}

uint16_t crc16Ccitt(const uint8_t *data, size_t length) {
  uint16_t crc = 0xffffu;
  for (size_t i = 0; i < length; ++i) {
    crc ^= static_cast<uint16_t>(data[i]) << 8;
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc & 0x8000u) != 0
          ? static_cast<uint16_t>((crc << 1) ^ 0x1021u)
          : static_cast<uint16_t>(crc << 1);
    }
  }
  return crc;
}

// START/END 的 14 字节头部包含格式和序号，帧数及负载长度均为零。
void buildControlPacket(Packet &packet, PacketType type, uint32_t sequence) {
  packet.bytes[0] = kMagic0;
  packet.bytes[1] = kMagic1;
  packet.bytes[2] = static_cast<uint8_t>(type);
  packet.bytes[3] = AudioConfig::CHANNELS;
  putLe16(packet.bytes + 4, static_cast<uint16_t>(AudioConfig::SAMPLE_RATE));
  putLe16(packet.bytes + 6, 0);
  putLe32(packet.bytes + 8, sequence);
  putLe16(packet.bytes + 12, 0);
  appendCrc(packet, PacketConfig::HEADER_BYTES);
}

// DATA 布局：A5 5A、类型、声道、采样率、帧数、序号、负载长度、PCM 负载。
void buildAudioPacket(Packet &packet, const int32_t *samples, size_t frames,
                      uint32_t sequence) {
  if (frames > PacketConfig::MAX_PACKET_FRAMES) {
    frames = PacketConfig::MAX_PACKET_FRAMES;
  }
  const size_t rawPayloadBytes =
      frames * AudioConfig::CHANNELS * AudioConfig::BYTES_PER_SAMPLE;
  packet.bytes[0] = kMagic0;
  packet.bytes[1] = kMagic1;
  packet.bytes[3] = AudioConfig::CHANNELS;
  putLe16(packet.bytes + 4, static_cast<uint16_t>(AudioConfig::SAMPLE_RATE));
  putLe16(packet.bytes + 6, static_cast<uint16_t>(frames));
  putLe32(packet.bytes + 8, sequence);

#if SINGLEMIC_ENABLE_HUFFMAN
  size_t huffmanPayloadBytes = 0;
  const bool useHuffman =
      encodeHuffmanPcm24(samples, frames,
                        packetBuildWorkspace.compressedPcm24,
                        sizeof(packetBuildWorkspace.compressedPcm24),
                        huffmanPayloadBytes);

  if (useHuffman) {
    memcpy(packet.bytes + PacketConfig::HEADER_BYTES,
           packetBuildWorkspace.compressedPcm24, huffmanPayloadBytes);
    packet.bytes[2] = static_cast<uint8_t>(PacketType::DATA_HUFFMAN);
    putLe16(packet.bytes + 12,
            static_cast<uint16_t>(huffmanPayloadBytes));
    appendCrc(packet, PacketConfig::HEADER_BYTES + huffmanPayloadBytes);
    return;
  }
#endif

  packet.bytes[2] = static_cast<uint8_t>(PacketType::DATA);
  putLe16(packet.bytes + 12, static_cast<uint16_t>(rawPayloadBytes));
  for (size_t i = 0; i < frames; ++i) {
    putLe24(packet.bytes + PacketConfig::HEADER_BYTES +
                AudioConfig::BYTES_PER_SAMPLE * i,
            samples[i]);
  }
  appendCrc(packet, PacketConfig::HEADER_BYTES + rawPayloadBytes);
}
}  // namespace AudioProtocol
