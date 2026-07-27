#ifndef XRPC_RPC_CHANNEL_H_
#define XRPC_RPC_CHANNEL_H_

#include <unistd.h>
#include <google/protobuf/service.h>

#include <memory>
#include <string>

#include "xrpc/codec/codec.h"

// 前向声明
class ZookeeperClient;

// 客户端 RPC 通道，继承自 google::protobuf::RpcChannel
// 负责序列化请求、发送到服务端、接收并反序列化响应
// 通过 _useXrpcProtocol 切换 Legacy / XRpc 协议
class RpcChannel : public google::protobuf::RpcChannel {
 public:
  // connectNow: 是否在构造时立即建立连接
  // useXrpc:   true=XRpc新协议, false=Legacy老协议
  explicit RpcChannel(bool connectNow, bool useXrpc = false);
  // 直连模式: 跳过 ZooKeeper 服务发现，直接连接指定地址
  RpcChannel(const std::string& ip, uint16_t port, bool useXrpc = false);
  virtual ~RpcChannel() {
    if (m_clientfd >= 0) {
      close(m_clientfd);
    }
  }

  void CallMethod(const ::google::protobuf::MethodDescriptor* method,
                  ::google::protobuf::RpcController* controller,
                  const ::google::protobuf::Message* request,
                  ::google::protobuf::Message* response,
                  ::google::protobuf::Closure* done) override;

 private:
  int m_clientfd;  // 客户端套接字文件描述符
  std::string service_name;
  std::string m_ip;
  uint16_t m_port;
  std::string method_name;
  int m_idx;  // 用于划分服务器 ip 和 port 的下标

  bool _useXrpcProtocol;
  std::unique_ptr<ClientCodec> _clientCodec;

  bool newConnect(const char* ip, uint16_t port);
  std::string QueryServiceHost(ZookeeperClient* zkclient,
                               std::string service_name,
                               std::string method_name, int& idx);

  // 确保读取指定长度的数据，解决 TCP 拆包问题
  ssize_t recv_exact(int fd, char* buf, size_t size);
};

#endif  // XRPC_RPC_CHANNEL_H_
