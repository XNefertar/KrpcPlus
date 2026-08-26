#ifndef XRPC_RPC_CHANNEL_H_
#define XRPC_RPC_CHANNEL_H_

#include <unistd.h>
#include <google/protobuf/service.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include "xrpc/codec/codec.h"

// 前向声明
class ZookeeperClient;
class ProtocolMessage;

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
  ~RpcChannel() override;

  void CallMethod(const ::google::protobuf::MethodDescriptor* method,
                  ::google::protobuf::RpcController* controller,
                  const ::google::protobuf::Message* request,
                  ::google::protobuf::Message* response,
                  ::google::protobuf::Closure* done) override;

 private:
  struct CallState {
    google::protobuf::Message* response = nullptr;
    google::protobuf::RpcController* controller = nullptr;
    google::protobuf::Closure* done = nullptr;
    std::chrono::steady_clock::time_point deadline;

    std::mutex mutex;
    std::condition_variable cv;
    bool completed = false;
  };

  std::atomic<int> m_clientfd;  // 客户端套接字文件描述符
  std::string m_ip;
  uint16_t m_port;
  int m_idx;  // 用于划分服务器 ip 和 port 的下标

  bool _useXrpcProtocol;
  std::unique_ptr<ClientCodec> _clientCodec;

  std::atomic<uint64_t> next_request_id_{1};
  std::atomic<bool> stopping_{false};
  std::atomic<bool> reader_running_{false};
  std::mutex connection_mutex_;
  std::mutex write_mutex_;
  std::thread reader_thread_;
  std::thread timeout_thread_;
  std::thread callback_thread_;

  std::mutex callback_mutex_;
  std::condition_variable callback_cv_;
  std::deque<google::protobuf::Closure*> callback_queue_;
  bool callbacks_stopping_ = false;

  std::mutex pending_mutex_;
  std::condition_variable pending_cv_;
  std::unordered_map<uint64_t, std::shared_ptr<CallState>> pending_calls_;

  bool newConnect(const char* ip, uint16_t port);
  bool EnsureXrpcConnection(const std::string& service_name,
                            const std::string& method_name,
                            google::protobuf::RpcController* controller);
  bool SendAll(int fd, const char* data, size_t size);
  bool ReadXrpcFrame(int fd, ProtocolMessage& response);
  void XrpcReaderLoop();
  void TimeoutLoop();
  void CallbackLoop();
  void DispatchCallback(google::protobuf::Closure* done);
  void CompleteCall(const std::shared_ptr<CallState>& call,
                    const ProtocolMessage* response,
                    const std::string& error);
  void FailAllPending(const std::string& error);
  void CallMethodXrpc(
      const ::google::protobuf::MethodDescriptor* method,
      ::google::protobuf::RpcController* controller,
      const ::google::protobuf::Message* request,
      ::google::protobuf::Message* response,
      ::google::protobuf::Closure* done);
  void CallMethodLegacy(
      const ::google::protobuf::MethodDescriptor* method,
      ::google::protobuf::RpcController* controller,
      const ::google::protobuf::Message* request,
      ::google::protobuf::Message* response);
  std::string QueryServiceHost(ZookeeperClient* zkclient,
                               std::string service_name,
                               std::string method_name, int& idx);

  // 确保读取指定长度的数据，解决 TCP 拆包问题
  ssize_t recv_exact(int fd, char* buf, size_t size);
};

#endif  // XRPC_RPC_CHANNEL_H_
