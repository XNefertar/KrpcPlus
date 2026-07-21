#pragma once

#include <arpa/inet.h>
#include <cstdint>
#include <cstring>

// ============================================================================
// XRPC 新一代协议 —— 固定帧头定义
// ============================================================================
namespace xrpc {

// ---- 魔数 ----
constexpr uint16_t kMagicNumber = 0x4B52;  // "KR" (little-endian storage, net order = 0x524B)

// ---- 尺寸常量 ----
constexpr size_t   kFixedHeaderSize  = 16;               // 固定头 16 字节
constexpr size_t   kMaxFrameSize     = 16 * 1024 * 1024; // 单帧上限 16 MB (防攻击)
constexpr size_t   kMinFrameSize     = kFixedHeaderSize; // 最小合法帧

// ---- 错误码 ----
constexpr int kCodecOk              =  0;
constexpr int kCodecIncomplete      =  1;  // CheckAndPick: 数据不完整，需要更多
constexpr int kCodecInvalidMagic    = -1;
constexpr int kCodecOversized       = -2;
constexpr int kCodecParseError      = -3;
constexpr int kCodecSerializeError  = -4;
constexpr int kCodecInternalError   = -5;

// ============================================================================
// 枚举：消息类型 / 流类型 / 序列化类型 / 压缩类型
// ============================================================================

enum class MessageType : uint8_t {
  kRequest  = 0,
  kResponse = 1,
  kOneway   = 2,  // 单向，不等待响应
};

enum class StreamType : uint8_t {
  kUnary        = 0,  // 一问一答
  kClientStream = 1,  // 客户端流
  kServerStream = 2,  // 服务端流
  kBidiStream   = 3,  // 双向流
};

enum class ContentType : int32_t {
  kProtobuf   = 0,
  kJson       = 1,
  kFlatBuffers = 2,
};

enum class ContentEncoding : int32_t {
  kNone   = 0,
  kGzip   = 1,
  kSnappy = 2,
  kLZ4    = 3,
};

// ============================================================================
// FixedHeader: 16 字节紧凑内存布局，与 wire format 逐字节对应
// ============================================================================
// 线缆格式（大端 / 网络字节序）:
// ┌──────────┬──────────┬──────────┬──────────┬──────────┬──────────┬──────────┐
// │  Magic   │  Type    │ Stream   │  Total   │  Header  │  Stream  │ Reserved │
// │  0xKR    │  (1B)    │  Type(1B)│  Size(4B)│  Size(2B)│  ID(4B)  │  (2B)    │
// │  (2B)    │          │          │          │          │          │          │
// └──────────┴──────────┴──────────┴──────────┴──────────┴──────────┴──────────┘

#pragma pack(push, 1)
struct FixedHeader {
  uint16_t magic        = 0;  // 魔数 0x4B52 (BE) 或校验错帧
  uint8_t  type         = 0;  // MessageType 枚举值
  uint8_t  stream_type  = 0;  // StreamType 枚举值
  uint32_t total_size   = 0;  // 整帧长度 = 16 + header_size + body_size (BE)
  uint16_t header_size  = 0;  // 可变头（protobuf）长度 (BE)
  uint32_t stream_id    = 0;  // 流 ID，用于多路复用 (BE)
  uint16_t reserved     = 0;  // 保留，必须为 0

  // ---------- 网络/主机序互转 ----------
  void HToN() {
    magic       = htons(magic);
    total_size  = htonl(total_size);
    header_size = htons(header_size);
    stream_id   = htonl(stream_id);
  }

  void NToH() {
    magic       = ntohs(magic);
    total_size  = ntohl(total_size);
    header_size = ntohs(header_size);
    stream_id   = ntohl(stream_id);
  }

  // ---------- 校验 ----------
  bool IsValid() const { return magic == kMagicNumber; }

  // ---------- 派生长度 ----------
  size_t BodySize() const {
    if (total_size < kFixedHeaderSize + header_size) return 0;
    return total_size - kFixedHeaderSize - header_size;
  }
};
#pragma pack(pop)

static_assert(sizeof(FixedHeader) == 16, "FixedHeader must be exactly 16 bytes");

}  // namespace xrpc
