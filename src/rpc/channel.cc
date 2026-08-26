#include "xrpc/rpc/channel.h"

#include <errno.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <unistd.h>

#include <chrono>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>
#include <limits>
#include <memory>

#include "xrpc/codec/codec_factory.h"
#include "xrpc/codec/noncontiguous_buffer.h"
#include "xrpc/codec/protocol_message.h"
#include "xrpc/codec/xrpc_codec.h"
#include "xrpc/common/application.h"
#include "xrpc/common/logger.h"
#include "xrpc/monitor/runtime_stats.h"
#include "xrpc/registry/load_balancer.h"
#include "xrpc/registry/route_manager.h"
#include "xrpc/registry/zookeeper_client.h"

using xrpc::FixedHeader;
using xrpc::kFixedHeaderSize;
using xrpc::kMaxFrameSize;

std::mutex g_data_mutx;  // 全局互斥锁，用于保护共享数据的线程安全

namespace {
constexpr std::chrono::milliseconds kDefaultRpcTimeout(1000);
}

// 辅助函数：循环读取直到读够 size 字节
ssize_t RpcChannel::recv_exact(int fd, char* buf, size_t size) {
  size_t total_read = 0;
  while (total_read < size) {
    ssize_t ret = recv(fd, buf + total_read, size - total_read, 0);
    if (ret == 0) return 0;    // 对端关闭
    if (ret == -1) {
      if (errno == EINTR) continue;  // 中断信号，继续读
      return -1;                      // 错误
    }
    total_read += ret;
  }
  return total_read;
}

// RPC 调用入口。XRpc 走 request-id 多路复用，Legacy 保留同步兼容路径。
void RpcChannel::CallMethod(
    const ::google::protobuf::MethodDescriptor* method,
    ::google::protobuf::RpcController* controller,
    const ::google::protobuf::Message* request,
    ::google::protobuf::Message* response,
    ::google::protobuf::Closure* done) {
  if (_useXrpcProtocol) {
    CallMethodXrpc(method, controller, request, response, done);
    return;
  }

  CallMethodLegacy(method, controller, request, response);
  if (done != nullptr) {
    done->Run();
  }
}

// Legacy 协议的响应没有 request_id，无法在同一连接上安全匹配并发请求。
void RpcChannel::CallMethodLegacy(
    const ::google::protobuf::MethodDescriptor* method,
    ::google::protobuf::RpcController* controller,
    const ::google::protobuf::Message* request,
    ::google::protobuf::Message* response) {
  // ===== [Stage 9] TOTAL: 整次调用总耗时 =====
  ScopedTimer total(RuntimeStats::TOTAL);

  // ---- 步骤 1: 懒初始化连接 + 服务发现 ----
  const std::string service_name(method->service()->name());
  const std::string method_name(method->name());

  if (-1 == m_clientfd) {
    // 直连模式: m_ip 已预设，直接 connect
    if (!m_ip.empty()) {
      ScopedTimer conn(RuntimeStats::CONNECT);
      if (!newConnect(m_ip.c_str(), m_port)) {
        controller->SetFailed("direct connect error");
        return;
      }
    } else {
      // ZK 模式: 服务发现 + 负载均衡 + 连接
      // [Stage 1] ZK_QUERY: ZooKeeper 连接 + 查路由表
      ZookeeperClient zkCli;
      {
        ScopedTimer zk(RuntimeStats::ZK_QUERY);
        zkCli.Start();
      }

      // 查询服务地址 (内部包含 getChildren + LB 选择)
      std::string host_data;
      {
        ScopedTimer zk(RuntimeStats::ZK_QUERY);
        host_data = QueryServiceHost(&zkCli, service_name, method_name, m_idx);
      }

      m_ip   = host_data.substr(0, m_idx);
      m_port = atoi(host_data.substr(m_idx + 1, host_data.size() - m_idx).c_str());
      std::cout << "ip: " << m_ip << ", port: " << m_port << std::endl;

      // [Stage 2] CONNECT: TCP 建连耗时
      bool connected;
      {
        ScopedTimer conn(RuntimeStats::CONNECT);
        connected = newConnect(m_ip.c_str(), m_port);
      }

      if (!connected) {
        LOG(ERROR) << "connect server error";
        controller->SetFailed("connect server error");
        return;
      }
      LOG(INFO) << "connect server success";
    }  // end ZK mode
  }

  // ---- 步骤 2: 序列化请求体 ----
  std::string args_str;
  {
    // [Stage 3] SERIALIZE_REQ: 请求体 Protobuf 序列化
    ScopedTimer ser(RuntimeStats::SERIALIZE_REQ);
    if (!request->SerializeToString(&args_str)) {
      controller->SetFailed("serialize request fail");
      return;
    }
  }

  // ---- 步骤 3: 构建 ProtocolMessage → Codec 编码 ----
  ProtocolMessage req_msg;
  req_msg.version       = _useXrpcProtocol ? ProtocolVersion::kXRpc : ProtocolVersion::kLegacy;
  req_msg.service_name  = service_name;
  req_msg.method_name   = method_name;
  req_msg.body          = args_str;
  req_msg.content_type  = ContentType::kProtobuf;
  req_msg.message_type  = MessageType::kRequest;

  ClientContext ctx;
  ctx.service_name = service_name;
  ctx.method_name  = method_name;

  std::string wire_data;
  {
    // [Stage 4] ENCODE_REQ: 协议编码 (Legacy/XRpc)
    ScopedTimer enc(RuntimeStats::ENCODE_REQ);
    NoncontiguousBuffer encoded;
    int enc_ret = _clientCodec->ZeroCopyEncode(ctx, req_msg, encoded);
    if (enc_ret != kCodecOk) {
      controller->SetFailed("encode request error");
      return;
    }
    wire_data = encoded.ToString();
  }

  // ---- 步骤 4: 网络收发 (仅 raw send/recv) ----
  std::vector<char> frame_buf;
  {
    // [Stage 5] NET_IO: 仅覆盖 send() + recv() 系统调用
    ScopedTimer io(RuntimeStats::NET_IO);

    // 4a. 发送
    if (-1 == send(m_clientfd, wire_data.data(), wire_data.size(), 0)) {
      close(m_clientfd);
      m_clientfd = -1;
      controller->SetFailed("send error");
      return;
    }

    // 4b. 接收响应帧 (协议相关读取)
    if (_useXrpcProtocol) {
      FixedHeader fh;
      ssize_t n = recv_exact(m_clientfd, (char*)&fh, kFixedHeaderSize);
      if (n != kFixedHeaderSize) {
        close(m_clientfd);
        m_clientfd = -1;
        controller->SetFailed("recv response header error");
        return;
      }
      fh.NToH();
      if (!fh.IsValid() || fh.total_size > kMaxFrameSize ||
          fh.total_size < kFixedHeaderSize) {
        close(m_clientfd);
        m_clientfd = -1;
        controller->SetFailed("invalid response frame header");
        return;
      }
      size_t remaining = fh.total_size - kFixedHeaderSize;
      frame_buf.resize(fh.total_size);
      std::memcpy(frame_buf.data(), &fh, kFixedHeaderSize);
      if (remaining > 0) {
        n = recv_exact(m_clientfd, frame_buf.data() + kFixedHeaderSize,
                       remaining);
        if (n != static_cast<ssize_t>(remaining)) {
          close(m_clientfd);
          m_clientfd = -1;
          controller->SetFailed("recv response body error");
          return;
        }
      }
    } else {
      uint32_t total_len_be = 0;
      if (recv_exact(m_clientfd, (char*)&total_len_be, 4) != 4) {
        close(m_clientfd);
        m_clientfd = -1;
        controller->SetFailed("recv response length error");
        return;
      }
      uint32_t total_len = ntohl(total_len_be);
      if (total_len > kMaxFrameSize) {
        close(m_clientfd);
        m_clientfd = -1;
        controller->SetFailed("response too large");
        return;
      }
      frame_buf.resize(4 + total_len);
      std::memcpy(frame_buf.data(), &total_len_be, 4);
      if (total_len > 0) {
        ssize_t n = recv_exact(m_clientfd, frame_buf.data() + 4, total_len);
        if (n != static_cast<ssize_t>(total_len)) {
          close(m_clientfd);
          m_clientfd = -1;
          controller->SetFailed("recv response body error");
          return;
        }
      }
    }
  }  // NET_IO timer 结束

  // ---- 步骤 5: Codec 解码响应 ----
  ProtocolMessage resp_msg;
  {
    // [Stage 6] DECODE_RES: 协议解码 (Legacy/XRpc)
    ScopedTimer dec(RuntimeStats::DECODE_RES);
    NoncontiguousBuffer resp_buf(frame_buf.data(), frame_buf.size());
    ClientContextPtr ctx_ptr = std::make_shared<ClientContext>(ctx);
    int dec_ret = _clientCodec->ZeroCopyDecode(ctx_ptr, resp_buf, resp_msg);
    if (dec_ret != kCodecOk) {
      close(m_clientfd);
      m_clientfd = -1;
      controller->SetFailed("decode response error, code=" +
                            std::to_string(dec_ret));
      return;
    }
  }

  // ---- 步骤 6: 反序列化到响应 Protobuf ----
  {
    // [Stage 7] DESERIALIZE_RES: 响应体 Protobuf 反序列化
    ScopedTimer des(RuntimeStats::DESERIALIZE_RES);
    if (!response->ParseFromString(resp_msg.body)) {
      close(m_clientfd);
      m_clientfd = -1;
      controller->SetFailed("parse response error");
      return;
    }
  }
}

void RpcChannel::CallMethodXrpc(
    const ::google::protobuf::MethodDescriptor* method,
    ::google::protobuf::RpcController* controller,
    const ::google::protobuf::Message* request,
    ::google::protobuf::Message* response,
    ::google::protobuf::Closure* done) {
  const std::string service_name(method->service()->name());
  const std::string method_name(method->name());

  auto fail_immediately = [&](const std::string& error) {
    if (controller != nullptr) controller->SetFailed(error);
    if (done != nullptr) done->Run();
  };

  if (!EnsureXrpcConnection(service_name, method_name, controller)) {
    if (done != nullptr) done->Run();
    return;
  }

  std::string args_str;
  {
    ScopedTimer timer(RuntimeStats::SERIALIZE_REQ);
    if (!request->SerializeToString(&args_str)) {
      fail_immediately("serialize request fail");
      return;
    }
  }

  const uint64_t request_id =
      next_request_id_.fetch_add(1, std::memory_order_relaxed);

  ProtocolMessage req_msg;
  req_msg.version = ProtocolVersion::kXRpc;
  req_msg.request_id = request_id;
  req_msg.stream_id = static_cast<uint32_t>(request_id);
  req_msg.service_name = service_name;
  req_msg.method_name = method_name;
  req_msg.body = std::move(args_str);
  req_msg.content_type = ContentType::kProtobuf;
  req_msg.message_type = MessageType::kRequest;
  req_msg.timeout = static_cast<uint32_t>(kDefaultRpcTimeout.count());

  ClientContext context;
  context.service_name = service_name;
  context.method_name = method_name;
  context.timeout_ms = req_msg.timeout;

  std::string wire_data;
  {
    ScopedTimer timer(RuntimeStats::ENCODE_REQ);
    NoncontiguousBuffer encoded;
    if (_clientCodec->ZeroCopyEncode(context, req_msg, encoded) != kCodecOk) {
      fail_immediately("encode request error");
      return;
    }
    wire_data = encoded.ToString();
  }

  auto call = std::make_shared<CallState>();
  call->response = response;
  call->controller = controller;
  call->done = done;
  call->deadline = std::chrono::steady_clock::now() + kDefaultRpcTimeout;

  bool admitted = false;
  {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    constexpr size_t kMaxInflightPerChannel = 4096;
    if (pending_calls_.size() < kMaxInflightPerChannel) {
      pending_calls_.emplace(request_id, call);
      admitted = true;
    }
  }
  if (!admitted) {
    fail_immediately("too many inflight rpc calls");
    return;
  }
  pending_cv_.notify_one();

  bool sent = false;
  {
    std::lock_guard<std::mutex> lock(write_mutex_);
    const int fd = m_clientfd.load(std::memory_order_acquire);
    sent = fd >= 0 && SendAll(fd, wire_data.data(), wire_data.size());
  }

  if (!sent) {
    std::shared_ptr<CallState> failed_call;
    {
      std::lock_guard<std::mutex> lock(pending_mutex_);
      auto it = pending_calls_.find(request_id);
      if (it != pending_calls_.end()) {
        failed_call = std::move(it->second);
        pending_calls_.erase(it);
      }
    }
    if (failed_call != nullptr) {
      CompleteCall(failed_call, nullptr, "send request error");
    }
    const int fd = m_clientfd.exchange(-1, std::memory_order_acq_rel);
    if (fd >= 0) {
      shutdown(fd, SHUT_RDWR);
      close(fd);
    }
  }

  // protobuf 的 done==nullptr 表示同步调用；同步只是等待同一异步内核的结果。
  if (done == nullptr) {
    std::unique_lock<std::mutex> lock(call->mutex);
    call->cv.wait(lock, [&] { return call->completed; });
  }
}

bool RpcChannel::EnsureXrpcConnection(
    const std::string& service_name, const std::string& method_name,
    google::protobuf::RpcController* controller) {
  std::lock_guard<std::mutex> lock(connection_mutex_);
  if (stopping_.load(std::memory_order_acquire)) {
    if (controller != nullptr) controller->SetFailed("rpc channel is stopping");
    return false;
  }

  if (m_clientfd.load(std::memory_order_acquire) < 0) {
    if (reader_thread_.joinable() &&
        !reader_running_.load(std::memory_order_acquire)) {
      reader_thread_.join();
    }

    if (m_ip.empty()) {
      ZookeeperClient zk_client;
      zk_client.Start();
      std::string host =
          QueryServiceHost(&zk_client, service_name, method_name, m_idx);
      const size_t separator = host.rfind(':');
      if (separator == std::string::npos || separator == 0 ||
          separator + 1 >= host.size()) {
        if (controller != nullptr) {
          controller->SetFailed("service discovery returned invalid address");
        }
        return false;
      }
      m_ip = host.substr(0, separator);
      m_port = static_cast<uint16_t>(std::stoi(host.substr(separator + 1)));
    }

    if (!newConnect(m_ip.c_str(), m_port)) {
      if (controller != nullptr) controller->SetFailed("connect server error");
      return false;
    }
  }

  if (!timeout_thread_.joinable()) {
    timeout_thread_ = std::thread(&RpcChannel::TimeoutLoop, this);
  }
  if (!callback_thread_.joinable()) {
    callback_thread_ = std::thread(&RpcChannel::CallbackLoop, this);
  }
  if (!reader_running_.load(std::memory_order_acquire)) {
    reader_running_.store(true, std::memory_order_release);
    reader_thread_ = std::thread(&RpcChannel::XrpcReaderLoop, this);
  }
  return true;
}

bool RpcChannel::SendAll(int fd, const char* data, size_t size) {
  size_t sent = 0;
  while (sent < size) {
    ssize_t result = send(fd, data + sent, size - sent, MSG_NOSIGNAL);
    if (result > 0) {
      sent += static_cast<size_t>(result);
      continue;
    }
    if (result < 0 && errno == EINTR) continue;
    return false;
  }
  return true;
}

bool RpcChannel::ReadXrpcFrame(int fd, ProtocolMessage& response) {
  char raw_header[kFixedHeaderSize];
  if (recv_exact(fd, raw_header, sizeof(raw_header)) !=
      static_cast<ssize_t>(sizeof(raw_header))) {
    return false;
  }

  FixedHeader header;
  std::memcpy(&header, raw_header, sizeof(header));
  header.NToH();
  if (!header.IsValid() || header.total_size > kMaxFrameSize ||
      header.total_size < kFixedHeaderSize ||
      header.header_size > header.total_size - kFixedHeaderSize) {
    return false;
  }

  std::vector<char> frame(header.total_size);
  std::memcpy(frame.data(), raw_header, sizeof(raw_header));
  const size_t remaining = header.total_size - kFixedHeaderSize;
  if (remaining > 0 &&
      recv_exact(fd, frame.data() + kFixedHeaderSize, remaining) !=
          static_cast<ssize_t>(remaining)) {
    return false;
  }

  NoncontiguousBuffer input(frame.data(), frame.size());
  ClientContextPtr context = std::make_shared<ClientContext>();
  return _clientCodec->ZeroCopyDecode(context, input, response) == kCodecOk;
}

void RpcChannel::XrpcReaderLoop() {
  const int fd = m_clientfd.load(std::memory_order_acquire);
  while (!stopping_.load(std::memory_order_acquire)) {
    ProtocolMessage response;
    if (fd < 0 || !ReadXrpcFrame(fd, response)) break;

    std::shared_ptr<CallState> call;
    {
      std::lock_guard<std::mutex> lock(pending_mutex_);
      auto it = pending_calls_.find(response.request_id);
      if (it != pending_calls_.end()) {
        call = std::move(it->second);
        pending_calls_.erase(it);
      }
    }
    if (call != nullptr) CompleteCall(call, &response, "");
  }

  std::unordered_map<uint64_t, std::shared_ptr<CallState>> failed_calls;
  {
    std::lock_guard<std::mutex> lock(connection_mutex_);
    int expected = fd;
    if (m_clientfd.compare_exchange_strong(expected, -1,
                                           std::memory_order_acq_rel)) {
      shutdown(fd, SHUT_RDWR);
      close(fd);
    }
    if (!stopping_.load(std::memory_order_acquire)) {
      std::lock_guard<std::mutex> pending_lock(pending_mutex_);
      failed_calls.swap(pending_calls_);
    }
    reader_running_.store(false, std::memory_order_release);
  }
  for (const auto& entry : failed_calls) {
    CompleteCall(entry.second, nullptr, "rpc connection closed");
  }
}

void RpcChannel::TimeoutLoop() {
  while (!stopping_.load(std::memory_order_acquire)) {
    std::vector<std::shared_ptr<CallState>> expired;
    {
      std::unique_lock<std::mutex> lock(pending_mutex_);
      pending_cv_.wait_for(lock, std::chrono::milliseconds(10), [&] {
        return stopping_.load(std::memory_order_acquire);
      });
      if (stopping_.load(std::memory_order_acquire)) break;

      const auto now = std::chrono::steady_clock::now();
      for (auto it = pending_calls_.begin(); it != pending_calls_.end();) {
        if (it->second->deadline <= now) {
          expired.push_back(std::move(it->second));
          it = pending_calls_.erase(it);
        } else {
          ++it;
        }
      }
    }
    for (const auto& call : expired) {
      CompleteCall(call, nullptr, "rpc timeout");
    }
  }
}

void RpcChannel::CallbackLoop() {
  while (true) {
    google::protobuf::Closure* done = nullptr;
    {
      std::unique_lock<std::mutex> lock(callback_mutex_);
      callback_cv_.wait(lock, [&] {
        return callbacks_stopping_ || !callback_queue_.empty();
      });
      if (callback_queue_.empty()) {
        if (callbacks_stopping_) break;
        continue;
      }
      done = callback_queue_.front();
      callback_queue_.pop_front();
    }
    done->Run();
  }
}

void RpcChannel::DispatchCallback(google::protobuf::Closure* done) {
  if (done == nullptr) return;
  {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    callback_queue_.push_back(done);
  }
  callback_cv_.notify_one();
}

void RpcChannel::CompleteCall(const std::shared_ptr<CallState>& call,
                              const ProtocolMessage* response,
                              const std::string& error) {
  google::protobuf::Closure* done = nullptr;
  {
    std::lock_guard<std::mutex> lock(call->mutex);
    if (call->completed) return;

    if (!error.empty()) {
      if (call->controller != nullptr) call->controller->SetFailed(error);
    } else if (response == nullptr || call->response == nullptr ||
               !call->response->ParseFromString(response->body)) {
      if (call->controller != nullptr) {
        call->controller->SetFailed("parse response error");
      }
    } else if (response->ret_code != 0 && call->controller != nullptr) {
      call->controller->SetFailed(response->error_msg.empty()
                                      ? "remote rpc error"
                                      : response->error_msg);
    }

    call->completed = true;
    done = call->done;
  }
  call->cv.notify_all();

  // 用户回调可能立即销毁 response/controller，所以之后不再访问 call 内容。
  DispatchCallback(done);
}

void RpcChannel::FailAllPending(const std::string& error) {
  std::unordered_map<uint64_t, std::shared_ptr<CallState>> pending;
  {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    pending.swap(pending_calls_);
  }
  for (const auto& entry : pending) {
    CompleteCall(entry.second, nullptr, error);
  }
}

RpcChannel::~RpcChannel() {
  stopping_.store(true, std::memory_order_release);
  pending_cv_.notify_all();

  const int fd = m_clientfd.exchange(-1, std::memory_order_acq_rel);
  if (fd >= 0) shutdown(fd, SHUT_RDWR);

  if (reader_thread_.joinable()) reader_thread_.join();
  if (fd >= 0) close(fd);
  if (timeout_thread_.joinable()) timeout_thread_.join();
  FailAllPending("rpc channel destroyed");
  {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    callbacks_stopping_ = true;
  }
  callback_cv_.notify_all();
  if (callback_thread_.joinable()) callback_thread_.join();
}

// 创建新的 socket 连接
bool RpcChannel::newConnect(const char* ip, uint16_t port) {
  int clientfd = socket(AF_INET, SOCK_STREAM, 0);
  if (-1 == clientfd) {
    char errtxt[512] = {0};
    std::cout << "socket error"
              << strerror_r(errno, errtxt, sizeof(errtxt)) << std::endl;
    LOG(ERROR) << "socket error:" << errtxt;
    return false;
  }

  int no_delay = 1;
  setsockopt(clientfd, IPPROTO_TCP, TCP_NODELAY, &no_delay,
             sizeof(no_delay));

  struct sockaddr_in server_addr;
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(port);
  server_addr.sin_addr.s_addr = inet_addr(ip);

  if (-1 ==
      connect(clientfd, (struct sockaddr*)&server_addr, sizeof(server_addr))) {
    close(clientfd);
    char errtxt[512] = {0};
    std::cout << "connect error"
              << strerror_r(errno, errtxt, sizeof(errtxt)) << std::endl;
    LOG(ERROR) << "connect server error" << errtxt;
    return false;
  }

  m_clientfd = clientfd;
  return true;
}

// 从 ZooKeeper 查询服务地址
std::string RpcChannel::QueryServiceHost(ZookeeperClient* zkclient,
                                          std::string service_name,
                                          std::string method_name, int& idx) {
  std::string method_path =
      "/" + service_name + "/" + method_name;
  std::cout << "method_path: " << method_path << std::endl;

  std::unique_lock<std::mutex> lock(g_data_mutx);

  // 先查本地路由表缓存
  std::vector<std::string> routeNodes =
      RouteManager::GetInstance()->GetRouteNodes(method_path);
  if (routeNodes.empty()) {
    // 本地缓存为空，同步去 ZK 拉取并注册 Watch
    RouteManager::GetInstance()->UpdateRouteTable(method_path, zkclient, true);
    routeNodes = RouteManager::GetInstance()->GetRouteNodes(method_path);
  }

  // 通过配置选取负载均衡策略
  std::string lb_type = Application::GetConfig().Load("loadbalancer");
  std::unique_ptr<LoadBalancer> loadBalancer;

  if (lb_type == "roundrobin") {
    loadBalancer = std::make_unique<RoundRobinLoadBalancer>();
  } else {
    loadBalancer = std::make_unique<RandomLoadBalancer>();
  }

  // [Stage 2] LOAD_BALANCE: 负载均衡选节点耗时
  std::string host_data_1;
  {
    ScopedTimer lb(RuntimeStats::LOAD_BALANCE);
    host_data_1 = loadBalancer->Select(routeNodes);
  }
  lock.unlock();

  if (host_data_1 == "") {
    LOG(ERROR) << method_path + " is not exist!";
    return " ";
  }

  idx = host_data_1.find(":");
  if (idx == -1) {
    LOG(ERROR) << method_path + " address is invalid!";
    return " ";
  }

  return host_data_1;
}

// 构造函数，支持延迟连接 + 协议选择
RpcChannel::RpcChannel(bool connectNow, bool useXrpc)
    : m_clientfd(-1), m_idx(0), _useXrpcProtocol(useXrpc) {
  // 根据协议版本创建对应 Codec
  _clientCodec = CodecFactory::CreateClientCodec(useXrpc);

  if (!connectNow) {
    return;
  }

  // 尝试连接服务器，最多重试 3 次
  auto rt = newConnect(m_ip.c_str(), m_port);
  int count = 3;
  while (!rt && count--) {
    rt = newConnect(m_ip.c_str(), m_port);
  }
}

// 直连模式构造函数: 跳过 ZK，直接连接指定地址
RpcChannel::RpcChannel(const std::string& ip, uint16_t port, bool useXrpc)
    : m_clientfd(-1), m_ip(ip), m_port(port), m_idx(0),
      _useXrpcProtocol(useXrpc) {
  _clientCodec = CodecFactory::CreateClientCodec(useXrpc);
  // 直连模式: 立即建立连接
  if (!newConnect(m_ip.c_str(), m_port)) {
    LOG(ERROR) << "Direct connect to " << ip << ":" << port << " failed";
  }
}
