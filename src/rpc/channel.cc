#include "xrpc/rpc/channel.h"

#include <errno.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <unistd.h>

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

std::mutex g_data_mutx;  // 全局互斥锁，用于保护共享数据的线程安全

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

// RPC 调用的核心方法 — 9 阶段全链路耗时统计
void RpcChannel::CallMethod(
    const ::google::protobuf::MethodDescriptor* method,
    ::google::protobuf::RpcController* controller,
    const ::google::protobuf::Message* request,
    ::google::protobuf::Message* response,
    ::google::protobuf::Closure* done) {
  // ===== [Stage 9] TOTAL: 整次调用总耗时 =====
  ScopedTimer total(RuntimeStats::TOTAL);

  // ---- 步骤 1: 懒初始化连接 + 服务发现 ----
  if (-1 == m_clientfd) {
    const google::protobuf::ServiceDescriptor* sd = method->service();
    service_name = sd->name();
    method_name = method->name();

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
