#ifndef SINGLEMIC_PACKET_PROTOCOL_H_
#define SINGLEMIC_PACKET_PROTOCOL_H_

#include <stddef.h>
#include <stdint.h>

#include "cfg.h"

// BLE 音频线协议实现。多字节数值均按小端序编码。
namespace AudioProtocol {
// 数据包类型与接收端约定一致，不能随意更改数值。
enum class PacketType : uint8_t {
  DATA = 1,
  START = 2,
  END = 3,
};

// 一个完整的 GATT 通知负载：实际长度由 length 指定，缓冲上限由配置决定。
struct Packet {
  uint16_t length = 0;
  uint8_t bytes[BLEConfig::MAX_PACKET_BYTES] = {};
};

// 根据 ATT MTU 和本项目头部长度计算一包可承载的 PCM 帧数。
size_t framesForMtu(uint16_t mtu);
// 写入小端序整数，避免依赖 MCU 内存字节序。
void putLe16(uint8_t *destination, uint16_t value);
void putLe32(uint8_t *destination, uint32_t value);
// 构造 START/END 控制包；控制包不包含 PCM 帧。
void buildControlPacket(Packet &packet, PacketType type, uint32_t sequence);
// 构造 DATA 包，并将 PCM 样本逐个序列化到包头后的负载区。
void buildAudioPacket(Packet &packet, const int16_t *samples, size_t frames,
                      uint32_t sequence);
}  // namespace AudioProtocol

#endif
