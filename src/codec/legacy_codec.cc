#include "xrpc/codec/legacy_codec.h"

#include <arpa/inet.h>
#include <cstring>

#include "xrpc/codec/protocol_message.h"
#include "xrpc/codec/xrpc_codec.h"
#include "xrpc/protocol/rpc_header.pb.h"

// ============================================================================
// LegacyServerCodec 实现
// ============================================================================
// 线缆格式: [4B TotalLen][4B HeaderLen][RpcHeader][Body]
// 所有多字节字段使用网络字节序 (Big Endian)

int LegacyServerCodec::CheckAndPick(const ConnectionPtr& /*conn*/,
                                     const NoncontiguousBuffer& in,
                                     std::any& /*metadata*/) {
  // 至少需要 4 字节读出 TotalLen
  if (in.ReadableBytes() < 4) {
    return 0;  // 数据不够
  }

  uint32_t total_len_be = 0;
  in.Peek(&total_len_be, 4);
  total_len_be = ntohl(total_len_be);

  // 上限校验（防攻击）
  if (total_len_be > kMaxFrameSize) {
    return kCodecOversized;
  }
  if (total_len_be < 4) {
    return kCodecParseError;  // 最小帧至少 4B 长度头
  }

  // 检查整帧是否到齐 (4B 长度头 + total_len 字节 payload)
  if (in.ReadableBytes() < 4 + total_len_be) {
    return 0;  // 半包，等待更多数据
  }

  return 4 + total_len_be;  // 完整帧大小
}

int LegacyServerCodec::ZeroCopyDecode(const ConnectionPtr& /*conn*/,
                                       NoncontiguousBuffer& in,
                                       ProtocolMessage& msg) {
  msg.Reset();
  msg.version = ProtocolVersion::kLegacy;

  // 1. 读 TotalLen
  uint32_t total_len_be = 0;
  in.Peek(&total_len_be, 4);
  uint32_t total_len = ntohl(total_len_be);

  // 2. 读 HeaderLen
  uint32_t header_len_be = 0;
  in.Peek(&header_len_be, 4, /*offset=*/4);
  uint32_t header_len = ntohl(header_len_be);

  // 3. 读 Header (RpcHeader protobuf)
  std::string header_bytes(header_len, '\0');
  in.Peek(&header_bytes[0], header_len, /*offset=*/8);

  xrpc::RpcHeader rpc_header;
  if (!rpc_header.ParseFromString(header_bytes)) {
    return kCodecParseError;
  }

  msg.service_name = rpc_header.service_name();
  msg.method_name  = rpc_header.method_name();
  msg.args_size    = rpc_header.args_size();

  // 4. 读 Body
  size_t body_len = total_len - 4 - header_len;
  if (body_len > 0) {
    msg.body.resize(body_len);
    in.Peek(&msg.body[0], body_len, /*offset=*/8 + header_len);
  }

  // 5. 消耗整帧
  in.Skip(4 + total_len);
  return kCodecOk;
}

int LegacyServerCodec::ZeroCopyEncode(const ConnectionPtr& /*conn*/,
                                       ProtocolMessage& msg,
                                       NoncontiguousBuffer& out) {
  // 服务端编码响应: [4B TotalLen][Body]
  uint32_t body_len = msg.body.size();
  uint32_t total_len = body_len;                 // 老协议响应: total_len = body_len
  uint32_t net_total = htonl(total_len);

  out.Append(&net_total, 4);
  if (body_len > 0) {
    out.Append(msg.body.data(), body_len);
  }
  return kCodecOk;
}

// ============================================================================
// LegacyClientCodec 实现
// ============================================================================

int LegacyClientCodec::ZeroCopyEncode(const ClientContext& ctx,
                                       ProtocolMessage& msg,
                                       NoncontiguousBuffer& out) {
  msg.version = ProtocolVersion::kLegacy;

  // 1. 构建 RpcHeader
  xrpc::RpcHeader rpc_header;
  rpc_header.set_service_name(msg.service_name.empty() ? ctx.service_name : msg.service_name);
  rpc_header.set_method_name(msg.method_name.empty() ? ctx.method_name : msg.method_name);
  rpc_header.set_args_size(msg.body.size());

  std::string header_str;
  if (!rpc_header.SerializeToString(&header_str)) {
    return kCodecSerializeError;
  }

  // 2. 计算长度并写入
  // 格式: [4B TotalLen] [4B HeaderLen] [Header] [Body]
  uint32_t header_len = header_str.size();
  uint32_t total_len  = 4 + header_len + msg.body.size();

  uint32_t net_total  = htonl(total_len);
  uint32_t net_header = htonl(header_len);

  out.Append(&net_total,  4);
  out.Append(&net_header, 4);
  out.Append(header_str);
  if (!msg.body.empty()) {
    out.Append(msg.body);
  }
  return kCodecOk;
}

int LegacyClientCodec::ZeroCopyDecode(const ClientContextPtr& /*ctx*/,
                                       NoncontiguousBuffer& in,
                                       ProtocolMessage& msg) {
  msg.Reset();
  msg.version = ProtocolVersion::kLegacy;

  // 客户端解码响应: [4B TotalLen][Body]
  if (in.ReadableBytes() < 4) {
    return kCodecIncomplete;
  }

  uint32_t total_len_be = 0;
  in.Peek(&total_len_be, 4);
  uint32_t total_len = ntohl(total_len_be);

  if (total_len > kMaxFrameSize) return kCodecOversized;
  if (in.ReadableBytes() < 4 + total_len) return kCodecIncomplete;

  msg.body.resize(total_len);
  in.Peek(&msg.body[0], total_len, /*offset=*/4);
  msg.message_type = MessageType::kResponse;

  in.Skip(4 + total_len);
  return kCodecOk;
}
