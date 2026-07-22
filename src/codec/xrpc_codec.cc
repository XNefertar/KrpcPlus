#include "xrpc/codec/xrpc_codec.h"

#include <arpa/inet.h>
#include <cstring>

#include "xrpc/codec/codec.h"
#include "xrpc/codec/connection_ptr.h"
#include "xrpc/codec/noncontiguous_buffer.h"
#include "xrpc/codec/protocol_message.h"
#include "xrpc/protocol/xrpc_protocol.pb.h"

// Bring xrpc namespace types into scope
using xrpc::ContentEncoding;
using xrpc::ContentType;
using xrpc::FixedHeader;
using xrpc::MessageType;
using xrpc::RequestProtocol;
using xrpc::ResponseProtocol;
using xrpc::StreamType;
using xrpc::kCodecIncomplete;
using xrpc::kCodecInternalError;
using xrpc::kCodecInvalidMagic;
using xrpc::kCodecOk;
using xrpc::kCodecOversized;
using xrpc::kCodecParseError;
using xrpc::kCodecSerializeError;
using xrpc::kFixedHeaderSize;
using xrpc::kMagicNumber;
using xrpc::kMaxFrameSize;
using xrpc::kMinFrameSize;

// ============================================================================
// XRpcServerCodec — 新一代协议服务端编解码器
// ============================================================================
class XRpcServerCodec : public ServerCodec {
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
// XRpcClientCodec — 新一代协议客户端编解码器
// ============================================================================
class XRpcClientCodec : public ClientCodec {
 public:
  int ZeroCopyEncode(const ClientContext& ctx,
                     ProtocolMessage& msg,
                     NoncontiguousBuffer& out) override;

  int ZeroCopyDecode(const ClientContextPtr& ctx,
                     NoncontiguousBuffer& in,
                     ProtocolMessage& msg) override;
};

// ============================================================================
// 辅助: 从 NoncontiguousBuffer 读 FixedHeader (不消费数据)
// ============================================================================
static int PeekFixedHeader(const NoncontiguousBuffer& in, FixedHeader& fh) {
  if (in.ReadableBytes() < kFixedHeaderSize) {
    return kCodecIncomplete;
  }
  // 逐字段读到本地 FixedHeader
  uint16_t buf16;
  uint32_t buf32;

  in.Peek(&buf16, 2, 0);                  // magic
  fh.magic = ntohs(buf16);

  in.Peek(&fh.type, 1, 2);               // type (1B, 不需要端序转换)
  in.Peek(&fh.stream_type, 1, 3);        // stream_type (1B)

  in.Peek(&buf32, 4, 4);                 // total_size
  fh.total_size = ntohl(buf32);

  in.Peek(&buf16, 2, 8);                 // header_size
  fh.header_size = ntohs(buf16);

  in.Peek(&buf32, 4, 10);               // stream_id
  fh.stream_id = ntohl(buf32);

  in.Peek(&fh.reserved, 2, 14);         // reserved

  return kCodecOk;
}

// ============================================================================
// XRpcServerCodec
// ============================================================================

int XRpcServerCodec::CheckAndPick(const ConnectionPtr& /*conn*/,
                                   const NoncontiguousBuffer& in,
                                   std::any& /*metadata*/) {
  if (in.ReadableBytes() < kFixedHeaderSize) {
    return 0;  // 数据不够，等待
  }

  FixedHeader fh;
  if (PeekFixedHeader(in, fh) != kCodecOk) {
    return 0;
  }

  // 校验魔数
  if (!fh.IsValid()) {
    return kCodecInvalidMagic;
  }

  // 校验 total_size
  if (fh.total_size > kMaxFrameSize || fh.total_size < kFixedHeaderSize) {
    return kCodecOversized;
  }

  // 校验 header_size 不溢出
  if (fh.header_size > fh.total_size - kFixedHeaderSize) {
    return kCodecParseError;
  }

  // 检查完整帧是否到齐
  if (in.ReadableBytes() < fh.total_size) {
    return 0;  // 半包等待
  }

  return fh.total_size;
}

int XRpcServerCodec::ZeroCopyDecode(const ConnectionPtr& /*conn*/,
                                     NoncontiguousBuffer& in,
                                     ProtocolMessage& msg) {
  msg.Reset();
  msg.version = ProtocolVersion::kXRpc;

  FixedHeader fh;
  if (PeekFixedHeader(in, fh) != kCodecOk) {
    return kCodecIncomplete;
  }

  if (!fh.IsValid()) return kCodecInvalidMagic;

  // --- 读取可变头 (Protobuf) ---
  std::string header_bytes(fh.header_size, '\0');
  in.Peek(&header_bytes[0], fh.header_size, /*offset=*/kFixedHeaderSize);

  // --- 读取 Body ---
  size_t body_len = fh.BodySize();
  if (body_len > 0) {
    msg.body.resize(body_len);
    in.Peek(&msg.body[0], body_len, /*offset=*/kFixedHeaderSize + fh.header_size);
  }

  // --- 解析可变头 ---
  if (fh.type == static_cast<uint8_t>(MessageType::kRequest)) {
    RequestProtocol req;
    if (!req.ParseFromString(header_bytes)) {
      return kCodecParseError;
    }
    msg.message_type  = MessageType::kRequest;
    msg.request_id    = req.request_id();
    msg.timeout       = req.timeout();
    msg.caller        = req.caller();
    msg.callee        = req.callee();
    msg.func_name     = req.func_name();
    msg.content_type  = static_cast<ContentType>(req.content_type());
    msg.content_encoding = static_cast<ContentEncoding>(req.content_encoding());
    msg.caller_app    = req.caller_app();
    for (const auto& kv : req.trans_info()) {
      msg.trans_info[kv.first] = kv.second;
    }
    // func_name 拆分出 service/method
    size_t slash = msg.func_name.rfind('/');
    if (slash != std::string::npos) {
      msg.service_name = msg.func_name.substr(0, slash);
      msg.method_name  = msg.func_name.substr(slash + 1);
    }
  } else if (fh.type == static_cast<uint8_t>(MessageType::kResponse)) {
    ResponseProtocol resp;
    if (!resp.ParseFromString(header_bytes)) {
      return kCodecParseError;
    }
    msg.message_type  = MessageType::kResponse;
    msg.request_id    = resp.request_id();
    msg.ret_code      = resp.ret_code();
    msg.error_msg     = resp.error_msg();
    msg.content_type  = static_cast<ContentType>(resp.content_type());
    msg.content_encoding = static_cast<ContentEncoding>(resp.content_encoding());
    for (const auto& kv : resp.trans_info()) {
      msg.trans_info[kv.first] = kv.second;
    }
  } else if (fh.type == static_cast<uint8_t>(MessageType::kOneway)) {
    msg.message_type = MessageType::kOneway;
  }

  msg.stream_type = static_cast<StreamType>(fh.stream_type);
  msg.stream_id   = fh.stream_id;

  // 消耗整帧
  in.Skip(fh.total_size);
  return kCodecOk;
}

int XRpcServerCodec::ZeroCopyEncode(const ConnectionPtr& /*conn*/,
                                     ProtocolMessage& msg,
                                     NoncontiguousBuffer& out) {
  // --- 构建可变头 ---
  ResponseProtocol resp;
  resp.set_request_id(msg.request_id);
  resp.set_ret_code(msg.ret_code);
  resp.set_error_msg(msg.error_msg);
  resp.set_content_type(static_cast<int32_t>(msg.content_type));
  resp.set_content_encoding(static_cast<int32_t>(msg.content_encoding));
  for (const auto& kv : msg.trans_info) {
    (*resp.mutable_trans_info())[kv.first] = kv.second;
  }

  std::string header_str;
  if (!resp.SerializeToString(&header_str)) {
    return kCodecSerializeError;
  }

  // --- 构建固定头 ---
  FixedHeader fh;
  fh.magic       = kMagicNumber;                           // 先写主机序
  fh.type        = static_cast<uint8_t>(MessageType::kResponse);
  fh.stream_type = static_cast<uint8_t>(msg.stream_type);
  fh.header_size = static_cast<uint16_t>(header_str.size());
  fh.stream_id   = msg.stream_id;
  fh.total_size  = kFixedHeaderSize + header_str.size() + msg.body.size();
  fh.reserved    = 0;

  // 检查上限
  if (fh.total_size > kMaxFrameSize) {
    return kCodecOversized;
  }

  fh.HToN();  // 转网络序

  // --- 写入 out buffer ---
  out.Append(&fh, kFixedHeaderSize);
  out.Append(header_str);
  if (!msg.body.empty()) {
    out.Append(msg.body);
  }
  return kCodecOk;
}

// ============================================================================
// XRpcClientCodec
// ============================================================================

int XRpcClientCodec::ZeroCopyEncode(const ClientContext& ctx,
                                     ProtocolMessage& msg,
                                     NoncontiguousBuffer& out) {
  // --- 构建可变头 ---
  RequestProtocol req;
  req.set_request_id(msg.request_id);
  req.set_timeout(msg.timeout > 0 ? msg.timeout : ctx.timeout_ms);
  req.set_caller(msg.caller.empty() ? ctx.caller : msg.caller);
  req.set_callee(msg.callee);
  req.set_func_name(msg.func_name.empty()
                        ? (msg.service_name + "/" + msg.method_name)
                        : msg.func_name);
  req.set_content_type(static_cast<int32_t>(msg.content_type));
  req.set_content_encoding(static_cast<int32_t>(msg.content_encoding));
  req.set_caller_app(msg.caller_app.empty() ? ctx.caller_app : msg.caller_app);

  if (msg.message_type == MessageType::kOneway) {
    req.set_message_type(2);
  } else {
    req.set_message_type(0);  // 请求
  }

  for (const auto& kv : msg.trans_info) {
    (*req.mutable_trans_info())[kv.first] = kv.second;
  }

  std::string header_str;
  if (!req.SerializeToString(&header_str)) {
    return kCodecSerializeError;
  }

  // --- 构建固定头 ---
  FixedHeader fh;
  fh.magic       = kMagicNumber;
  fh.type        = static_cast<uint8_t>(msg.message_type);
  fh.stream_type = static_cast<uint8_t>(msg.stream_type);
  fh.header_size = static_cast<uint16_t>(header_str.size());
  fh.stream_id   = msg.stream_id;
  fh.total_size  = kFixedHeaderSize + header_str.size() + msg.body.size();
  fh.reserved    = 0;

  if (fh.total_size > kMaxFrameSize) {
    return kCodecOversized;
  }
  fh.HToN();

  out.Append(&fh, kFixedHeaderSize);
  out.Append(header_str);
  if (!msg.body.empty()) {
    out.Append(msg.body);
  }
  return kCodecOk;
}

int XRpcClientCodec::ZeroCopyDecode(const ClientContextPtr& /*ctx*/,
                                     NoncontiguousBuffer& in,
                                     ProtocolMessage& msg) {
  msg.Reset();
  msg.version = ProtocolVersion::kXRpc;
  msg.message_type = MessageType::kResponse;

  if (in.ReadableBytes() < kFixedHeaderSize) {
    return kCodecIncomplete;
  }

  FixedHeader fh;
  if (PeekFixedHeader(in, fh) != kCodecOk) {
    return kCodecIncomplete;
  }

  if (!fh.IsValid()) return kCodecInvalidMagic;

  if (fh.total_size > kMaxFrameSize) return kCodecOversized;
  if (in.ReadableBytes() < fh.total_size) return kCodecIncomplete;

  // 读可变头
  std::string header_bytes(fh.header_size, '\0');
  in.Peek(&header_bytes[0], fh.header_size, /*offset=*/kFixedHeaderSize);

  // 读 Body
  size_t body_len = fh.BodySize();
  if (body_len > 0) {
    msg.body.resize(body_len);
    in.Peek(&msg.body[0], body_len, /*offset=*/kFixedHeaderSize + fh.header_size);
  }

  // 解析可变头
  ResponseProtocol resp;
  if (!resp.ParseFromString(header_bytes)) {
    return kCodecParseError;
  }

  msg.request_id    = resp.request_id();
  msg.ret_code      = resp.ret_code();
  msg.error_msg     = resp.error_msg();
  msg.content_type  = static_cast<ContentType>(resp.content_type());
  msg.content_encoding = static_cast<ContentEncoding>(resp.content_encoding());
  for (const auto& kv : resp.trans_info()) {
    msg.trans_info[kv.first] = kv.second;
  }

  msg.stream_type = static_cast<StreamType>(fh.stream_type);
  msg.stream_id   = fh.stream_id;

  in.Skip(fh.total_size);
  return kCodecOk;
}

// ============================================================================
// 工厂辅助函数 (供 CodecFactory 使用)
// ============================================================================
std::unique_ptr<ServerCodec> CreateXRpcServerCodec() {
  return std::make_unique<XRpcServerCodec>();
}
std::unique_ptr<ClientCodec> CreateXRpcClientCodec() {
  return std::make_unique<XRpcClientCodec>();
}
