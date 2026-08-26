#ifndef XRPC_RPC_SERVER_H_
#define XRPC_RPC_SERVER_H_

#include <google/protobuf/descriptor.h>
#include <google/protobuf/service.h>
#include <muduo/net/EventLoop.h>
#include <muduo/net/InetAddress.h>
#include <muduo/net/TcpConnection.h>
#include <muduo/net/TcpServer.h>

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "xrpc/codec/codec.h"
#include "xrpc/codec/protocol_message.h"

// 前向声明
class ZookeeperClient;

// RPC 服务端，负责发布 RPC 服务、接收和处理 RPC 请求
class RpcServer {
 public:
  // 注册 protobuf Service 对象，发布其 RPC 方法
  void NotifyService(google::protobuf::Service* service);
  ~RpcServer();

  // 启动 RPC 服务节点，开始提供远程网络调用服务
  void Run();

 private:
  muduo::net::EventLoop event_loop;

  struct ServiceInfo {
    google::protobuf::Service* service;
    std::unordered_map<std::string, const google::protobuf::MethodDescriptor*>
        method_map;
  };

  // 保存已注册的服务对象和 RPC 方法
  std::unordered_map<std::string, ServiceInfo> service_map;

  // Codec 层：自动检测协议版本并解码/编码
  std::unique_ptr<ServerCodec> _serverCodec;
  std::once_flag codec_init_flag_;

  void OnConnection(const muduo::net::TcpConnectionPtr& conn);
  void OnMessage(const muduo::net::TcpConnectionPtr& conn,
                 muduo::net::Buffer* buffer, muduo::Timestamp receive_time);
  void SendRpcResponse(const muduo::net::TcpConnectionPtr& conn,
                       google::protobuf::Message* response,
                       const ProtocolMessage& request_metadata);
};

#endif  // XRPC_RPC_SERVER_H_
