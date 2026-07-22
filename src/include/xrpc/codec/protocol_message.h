#pragma once

#include <map>
#include <string>
#include "xrpc/codec/xrpc_codec.h"

using xrpc::kCodecOk;
using xrpc::ContentEncoding;
using xrpc::ContentType;
using xrpc::MessageType;
using xrpc::StreamType;

// ============================================================================
// ProtocolMessage — 统一协议消息载体
// ============================================================================
// 描述：同时承载 Legacy 和 XRpc 两种协议的元信息 + body，
//        使 Codec 层对上层透明 —— 上层只读写 ProtocolMessage。
//
// 使用方式：
//   - 编码时：填充相应字段 → codec->Encode(msg, out)
//   - 解码时：codec->Decode(in, msg) → 读取字段
// ============================================================================

enum class ProtocolVersion {
  kLegacy,  // [4B TotalLen][4B HeaderLen][RpcHeader][Body]
  kXRpc,    // [16B FixedHeader][RequestProtocol/ResponseProtocol][Body]
};

class ProtocolMessage {
 public:
  // ---- 协议版本 ----
  ProtocolVersion version = ProtocolVersion::kXRpc;

  // ========== 公共字段 ==========
  uint64_t         request_id      = 0;
  ContentType      content_type    = ContentType::kProtobuf;
  ContentEncoding  content_encoding = ContentEncoding::kNone;
  std::string      body;            // 请求/响应体序列化字节

  // ========== Legacy 协议字段 ==========
  std::string service_name;
  std::string method_name;
  uint32_t    args_size = 0;       // RpcHeader.args_size

  // ========== XRpc 协议字段 ==========
  MessageType message_type  = MessageType::kRequest;
  StreamType  stream_type   = StreamType::kUnary;
  uint32_t    stream_id     = 0;
  uint32_t    timeout       = 1000;  // ms
  std::string caller;
  std::string callee;
  std::string func_name;            // 如 "/UserService/Login"
  std::string caller_app;

  // Response 专用
  int32_t     ret_code  = 0;
  std::string error_msg;

  // 透传信息 (tracing / 染色 / 自定义标签)
  std::map<std::string, std::string> trans_info;

  // ---- 便捷方法 ----
  void Reset();
  bool IsRequest()  const { return message_type == MessageType::kRequest; }
  bool IsResponse() const { return message_type == MessageType::kResponse; }
  bool IsOneway()   const { return message_type == MessageType::kOneway; }
};
