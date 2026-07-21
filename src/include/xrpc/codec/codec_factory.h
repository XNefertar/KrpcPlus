#pragma once

#include <memory>
#include "xrpc/codec/codec.h"

// ============================================================================
// CodecFactory — 编解码器工厂
// ============================================================================
// 职责:
//   1. 根据 buffer 前 2 字节 Magic 自动识别协议版本
//   2. 创建对应的 ServerCodec / ClientCodec
//   3. 支持手动指定协议版本（跳过自动检测）
//
// 路由逻辑:
//   前 2 字节 == 0x4B52 ("KR" BE) → XRpcCodec (新协议)
//   否则                        → LegacyCodec (老协议, 兼容旧客户端)
// ============================================================================

class CodecFactory {
 public:
  // ---- 自动检测：根据 buffer 内容返回对应 ServerCodec ----
  static std::unique_ptr<ServerCodec> CreateServerCodec(const NoncontiguousBuffer& in);

  // ---- 手动指定 ----
  static std::unique_ptr<ServerCodec> CreateServerCodec(bool useXrpc);
  static std::unique_ptr<ClientCodec> CreateClientCodec(bool useXrpc);

 private:
  CodecFactory() = default;
};
