#ifndef ICS_TEST_PACKET_PROTOCOL_H_
#define ICS_TEST_PACKET_PROTOCOL_H_

#include <stdint.h>

#include "cfg.h"

namespace IcsTestProtocol {

// 接收端解析的固定 14 字节小端包头和单声道 PCM16 负载。
enum PacketType : uint8_t { DATA = 1, START = 2, END = 3 };

struct BlePacket {
  uint16_t length;
  uint8_t bytes[IcsTestConfig::MAX_PACKET_BYTES];
};

void buildControlPacket(PacketType type, uint32_t sequence, BlePacket &packet);
void buildAudioPacket(const float *samples, uint32_t &sequence,
                      BlePacket &packet);

}  // namespace IcsTestProtocol

#endif
