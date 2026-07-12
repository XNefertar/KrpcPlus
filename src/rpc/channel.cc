#include "xrpc/rpc/channel.h"

#include <errno.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <memory>

#include "xrpc/common/application.h"
#include "xrpc/common/logger.h"
#include "xrpc/monitor/runtime_stats.h"
#include "xrpc/registry/load_balancer.h"
#include "xrpc/registry/route_manager.h"
#include "xrpc/registry/zookeeper_client.h"
#include "xrpc/protocol/rpc_header.pb.h"

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

// RPC 调用的核心方法，负责将客户端请求序列化并发送到服务端，同时接收响应
void RpcChannel::CallMethod(
    const ::google::protobuf::MethodDescriptor* method,
    ::google::protobuf::RpcController* controller,
    const ::google::protobuf::Message* request,
    ::google::protobuf::Message* response,
    ::google::protobuf::Closure* done) {
  ScopedTimer total(RuntimeStats::TOTAL);
  if (-1 == m_clientfd) {  // 如果客户端 socket 未初始化
    // 获取服务对象名和方法名
    const google::protobuf::ServiceDescriptor* sd = method->service();
    service_name = sd->name();
    method_name = method->name();

    // 客户端需要查询 ZooKeeper，找到提供该服务的服务器地址
    ZookeeperClient zkCli;
    zkCli.Start();
    std::string host_data;
    {
      ScopedTimer zk(RuntimeStats::ZK_QUERY);
      host_data = QueryServiceHost(&zkCli, service_name, method_name, m_idx);
    }
    m_ip = host_data.substr(0, m_idx);
    std::cout << "ip: " << m_ip << std::endl;
    m_port = atoi(
        host_data.substr(m_idx + 1, host_data.size() - m_idx).c_str());
    std::cout << "port: " << m_port << std::endl;

    // 尝试连接服务器
    auto rt = newConnect(m_ip.c_str(), m_port);
    if (!rt) {
      LOG(ERROR) << "connect server error";
      return;
    } else {
      LOG(INFO) << "connect server success";
    }
  }

  // 2. 序列化请求参数
  std::string args_str;
  {
    ScopedTimer ser(RuntimeStats::SERIALIZE_REQ);
    if (!request->SerializeToString(&args_str)) {
      controller->SetFailed("serialize request fail");
      return;
    }
  }

  // 3. 构建协议头
  xrpc::RpcHeader rpc_header;
  rpc_header.set_service_name(service_name);
  rpc_header.set_method_name(method_name);
  rpc_header.set_args_size(args_str.size());

  std::string rpc_header_str;
  if (!rpc_header.SerializeToString(&rpc_header_str)) {
    controller->SetFailed("serialize rpc header error!");
    return;
  }

  // 4. 打包数据发送
  // 格式：[4B Total Len] + [4B Header Len] + [Header] + [Args]
  uint32_t header_size = rpc_header_str.size();
  uint32_t total_len = 4 + header_size + args_str.size();

  // 转网络字节序
  uint32_t net_total_len = htonl(total_len);
  uint32_t net_header_len = htonl(header_size);

  std::string send_rpc_str;
  send_rpc_str.reserve(4 + 4 + header_size + args_str.size());

  send_rpc_str.append((char*)&net_total_len, 4);
  send_rpc_str.append((char*)&net_header_len, 4);
  send_rpc_str.append(rpc_header_str);
  send_rpc_str.append(args_str);

  // 发送
  {
    ScopedTimer io(RuntimeStats::NET_IO);
    if (-1 == send(m_clientfd, send_rpc_str.c_str(), send_rpc_str.size(), 0)) {
      close(m_clientfd);
      m_clientfd = -1;
      controller->SetFailed("send error");
      return;
    }

    // 5. 接收响应
    // 格式：[4B Total Len] + [Response Data]

    // A. 先读 4 字节长度头
    uint32_t response_len = 0;
    if (recv_exact(m_clientfd, (char*)&response_len, 4) != 4) {
      close(m_clientfd);
      m_clientfd = -1;
      controller->SetFailed("recv response length error");
      return;
    }
    response_len = ntohl(response_len);

    // B. 根据长度读取 Body
    std::vector<char> recv_buf(response_len);
    if (recv_exact(m_clientfd, recv_buf.data(), response_len) != response_len) {
      close(m_clientfd);
      m_clientfd = -1;
      controller->SetFailed("recv response body error");
      return;
    }

    // 6. 反序列化响应
    {
      ScopedTimer des(RuntimeStats::DESERIALIZE_RES);
      if (!response->ParseFromArray(recv_buf.data(), response_len)) {
        close(m_clientfd);
        m_clientfd = -1;
        controller->SetFailed("parse response error");
        return;
      }
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
    // 默认使用随机负载均衡
    loadBalancer = std::make_unique<RandomLoadBalancer>();
  }

  std::string host_data_1 = loadBalancer->Select(routeNodes);
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

// 构造函数，支持延迟连接
RpcChannel::RpcChannel(bool connectNow) : m_clientfd(-1), m_idx(0) {
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
