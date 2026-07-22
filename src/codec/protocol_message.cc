#include "xrpc/codec/protocol_message.h"

void ProtocolMessage::Reset() {
  version          = ProtocolVersion::kXRpc;
  request_id       = 0;
  content_type     = ContentType::kProtobuf;
  content_encoding = ContentEncoding::kNone;
  body.clear();
  service_name.clear();
  method_name.clear();
  args_size        = 0;
  message_type     = MessageType::kRequest;
  stream_type      = StreamType::kUnary;
  stream_id        = 0;
  timeout          = 1000;
  caller.clear();
  callee.clear();
  func_name.clear();
  caller_app.clear();
  ret_code         = 0;
  error_msg.clear();
  trans_info.clear();
}
