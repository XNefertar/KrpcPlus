#include "xrpc/codec/codec_factory.h"

#include <cstring>

#include "xrpc/codec/legacy_codec.h"
#include "xrpc/codec/noncontiguous_buffer.h"
#include "xrpc/codec/xrpc_codec.h"

// 前向声明（实现在 xrpc_codec.cc 中）
namespace xrpc {
std::unique_ptr<ServerCodec> CreateXRpcServerCodec();
std::unique_ptr<ClientCodec> CreateXRpcClientCodec();
}  // namespace xrpc

// ============================================================================
// CodecFactory 实现
// ============================================================================

std::unique_ptr<ServerCodec> CodecFactory::CreateServerCodec(
    const NoncontiguousBuffer& in) {
  // 读取前 2 字节 Magic 做协议识别
  if (in.ReadableBytes() >= 2) {
    uint16_t magic_be = 0;
    in.Peek(&magic_be, 2);
    uint16_t magic = ntohs(magic_be);
    if (magic == xrpc::kMagicNumber) {
      return xrpc::CreateXRpcServerCodec();
    }
  }
  // 不满足新协议魔数 → 回退 Legacy
  return std::make_unique<LegacyServerCodec>();
}

std::unique_ptr<ServerCodec> CodecFactory::CreateServerCodec(bool useXrpc) {
  if (useXrpc) {
    return xrpc::CreateXRpcServerCodec();
  }
  return std::make_unique<LegacyServerCodec>();
}

std::unique_ptr<ClientCodec> CodecFactory::CreateClientCodec(bool useXrpc) {
  if (useXrpc) {
    return xrpc::CreateXRpcClientCodec();
  }
  return std::make_unique<LegacyClientCodec>();
}
