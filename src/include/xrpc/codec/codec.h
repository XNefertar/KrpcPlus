#pragma once

#include <any>

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