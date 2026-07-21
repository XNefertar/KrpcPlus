#pragma once

#include <any>
#include <memory>

#include "xrpc/codec/connection_ptr.h"

// 前置声明（避免循环依赖，完整定义在各自头文件中）
class NoncontiguousBuffer;
class ProtocolMessage;

// ============================================================================
// ServerCodec — 服务端编解码器接口
// ============================================================================
// CheckAndPick: 检查 buffer 中是否有完整帧，返回:
//   >0: 帧的总字节数
//    0: 数据不完整，等待更多数据
//   <0: 非法数据（magic/长度异常），应断开连接
// ZeroCopyDecode: 将完整帧解码为 ProtocolMessage
// ZeroCopyEncode: 将 ProtocolMessage 编码写入 out buffer

class ServerCodec {
public:
    virtual ~ServerCodec() = default;

    virtual int CheckAndPick(const ConnectionPtr& conn,
                             const NoncontiguousBuffer& in,
                             std::any& metadata) = 0;

    virtual int ZeroCopyDecode(const ConnectionPtr& conn,
                               NoncontiguousBuffer& in,
                               ProtocolMessage& msg) = 0;

    virtual int ZeroCopyEncode(const ConnectionPtr& conn,
                               ProtocolMessage& msg,
                               NoncontiguousBuffer& out) = 0;
};

// ============================================================================
// ClientCodec — 客户端编解码器接口
// ============================================================================

class ClientCodec {
public:
    virtual ~ClientCodec() = default;

    virtual int ZeroCopyEncode(const ClientContext& ctx,
                               ProtocolMessage& msg,
                               NoncontiguousBuffer& out) = 0;

    virtual int ZeroCopyDecode(const ClientContextPtr& ctx,
                               NoncontiguousBuffer& in,
                               ProtocolMessage& msg) = 0;
};