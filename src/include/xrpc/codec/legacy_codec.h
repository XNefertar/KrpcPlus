#pragma once

#include "xrpc/codec/codec.h"

// ============================================================================
// LegacyServerCodec — 老协议编解码器（服务端）
// ============================================================================
// 线缆格式:
//   [4B TotalLen (BE)] [4B HeaderLen (BE)] [Header (RpcHeader protobuf)] [Body]
// 其中 RpcHeader: service_name, method_name, args_size

class LegacyServerCodec : public ServerCodec {
 public:
  int CheckAndPick(const ConnectionPtr& conn,
                   const NoncontiguousBuffer& in,
                   std::any& metadata) override;

  int ZeroCopyDecode(const ConnectionPtr& conn,
                     NoncontiguousBuffer& in,
                     ProtocolMessage& msg) override;

  int ZeroCopyEncode(const ConnectionPtr& conn,
                     ProtocolMessage& msg,
                     NoncontiguousBuffer& out) override;
};

// ============================================================================
// LegacyClientCodec — 老协议编解码器（客户端）
// ============================================================================

class LegacyClientCodec : public ClientCodec {
 public:
  int ZeroCopyEncode(const ClientContext& ctx,
                     ProtocolMessage& msg,
                     NoncontiguousBuffer& out) override;

  int ZeroCopyDecode(const ClientContextPtr& ctx,
                     NoncontiguousBuffer& in,
                     ProtocolMessage& msg) override;
};
