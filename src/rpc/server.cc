#include "xrpc/rpc/server.h"

#include <iostream>

#include "xrpc/common/application.h"
#include "xrpc/common/logger.h"
#include "xrpc/protocol/rpc_header.pb.h"
#include "xrpc/registry/zookeeper_client.h"

// 注册服务对象及其方法，以便服务端能够处理客户端的 RPC 请求
void RpcServer::NotifyService(google::protobuf::Service* service) {
  ServiceInfo service_info;

  const google::protobuf::ServiceDescriptor* psd = service->GetDescriptor();

  // 获取服务的名字
  std::string service_name = psd->name();
  // 获取服务端对象 service 的方法数量
  int method_count = psd->method_count();

  std::cout << "service_name=" << service_name << std::endl;

  // 遍历服务中的所有方法，并注册到服务信息中
  for (int i = 0; i < method_count; ++i) {
    const google::protobuf::MethodDescriptor* pmd = psd->method(i);
    std::string method_name = pmd->name();
    std::cout << "method_name=" << method_name << std::endl;
    service_info.method_map.emplace(method_name, pmd);
  }
  service_info.service = service;
  service_map.emplace(service_name, service_info);
}

// 启动 RPC 服务节点，开始提供远程网络调用服务
void RpcServer::Run() {
  // 读取配置文件中的 RPC 服务器 IP 和端口
  std::string ip =
      Application::GetInstance().GetConfig().Load("rpcserverip");
  int port = atoi(
      Application::GetInstance().GetConfig().Load("rpcserverport").c_str());

  // 使用 muduo 网络库，创建地址对象
  muduo::net::InetAddress address(ip, port);

  // 创建 TcpServer 对象
  std::shared_ptr<muduo::net::TcpServer> server =
      std::make_shared<muduo::net::TcpServer>(&event_loop, address,
                                              "RpcServer");

  // 绑定连接回调和消息回调
  server->setConnectionCallback(
      std::bind(&RpcServer::OnConnection, this, std::placeholders::_1));
  server->setMessageCallback(
      std::bind(&RpcServer::OnMessage, this, std::placeholders::_1,
                std::placeholders::_2, std::placeholders::_3));

  // 设置 muduo 库的线程数量
  server->setThreadNum(4);

  // 将当前 RPC 节点上要发布的服务全部注册到 ZooKeeper 上
  ZookeeperClient zkclient;
  zkclient.Start();
  for (auto& sp : service_map) {
    std::string service_path = "/" + sp.first;
    zkclient.Create(service_path.c_str(), nullptr, 0);
    for (auto& mp : sp.second.method_map) {
      std::string method_path = service_path + "/" + mp.first;
      char method_path_data[128] = {0};
      sprintf(method_path_data, "%s:%d", ip.c_str(), port);

      // 先确保该方法的父节点（非临时）存在
      zkclient.Create(method_path.c_str(), nullptr, 0);

      // 将 IP:Port 挂载为临时子节点
      std::string route_path = method_path + "/" + method_path_data;
      // ZOO_EPHEMERAL 表示临时节点，客户端断开后 ZK 自动删除
      zkclient.Create(route_path.c_str(), method_path_data,
                      strlen(method_path_data), ZOO_EPHEMERAL);
    }
  }

  // RPC 服务端准备启动
  std::cout << "RpcServer start service at ip:" << ip << " port:" << port
            << std::endl;

  // 启动网络服务
  server->start();
  event_loop.loop();
}

// 连接回调函数，处理客户端连接事件
void RpcServer::OnConnection(const muduo::net::TcpConnectionPtr& conn) {
  if (!conn->connected()) {
    conn->shutdown();
  }
}

// 消息回调函数，处理客户端发送的 RPC 请求
void RpcServer::OnMessage(const muduo::net::TcpConnectionPtr& conn,
                           muduo::net::Buffer* buffer,
                           muduo::Timestamp receive_time) {
  std::cout << "OnMessage" << std::endl;

  // 循环处理缓冲区，解决粘包问题
  while (buffer->readableBytes() >= 4) {
    // 1. 预读取前 4 个字节（Total Length）
    uint32_t total_len = 0;
    std::memcpy(&total_len, buffer->peek(), 4);
    total_len = ntohl(total_len);

    // 2. 检查数据是否完整（拆包处理）
    if (buffer->readableBytes() < 4 + total_len) {
      break;
    }

    // --- 数据包完整，开始解包 ---

    // 3. 真正读取数据
    buffer->retrieve(4);  // 消耗掉长度头

    // 读取 Header Length
    uint32_t header_len = 0;
    const char* data_ptr = buffer->peek();
    std::memcpy(&header_len, data_ptr, 4);
    header_len = ntohl(header_len);
    buffer->retrieve(4);  // 消耗掉 header length

    // 读取 Header 数据
    std::string rpc_header_str(buffer->peek(), header_len);
    xrpc::RpcHeader xrpcHeader;
    buffer->retrieve(header_len);

    // 读取 Body 数据 (args)
    uint32_t args_size =
        total_len - 4 - header_len;  // 总长度 - header长度字段(4) - header内容
    std::string args_str(buffer->peek(), args_size);
    buffer->retrieve(args_size);

    // 4. 业务逻辑处理
    if (!xrpcHeader.ParseFromString(rpc_header_str)) {
      std::cout << "header parse error" << std::endl;
      return;
    }

    std::string service_name = xrpcHeader.service_name();
    std::string method_name = xrpcHeader.method_name();

    auto it = service_map.find(service_name);
    if (it == service_map.end()) {
      std::cout << service_name << " is not exist!" << std::endl;
      return;
    }
    auto mit = it->second.method_map.find(method_name);
    if (mit == it->second.method_map.end()) {
      std::cout << service_name << "." << method_name << " is not exist!"
                << std::endl;
      return;
    }

    google::protobuf::Service* service = it->second.service;
    const google::protobuf::MethodDescriptor* method = mit->second;

    google::protobuf::Message* request =
        service->GetRequestPrototype(method).New();
    if (!request->ParseFromString(args_str)) {
      std::cout << "request parse error" << std::endl;
      return;
    }
    google::protobuf::Message* response =
        service->GetResponsePrototype(method).New();

    google::protobuf::Closure* done = google::protobuf::NewCallback<
        RpcServer, const muduo::net::TcpConnectionPtr&,
        google::protobuf::Message*>(this, &RpcServer::SendRpcResponse, conn,
                                     response);
    service->CallMethod(method, nullptr, request, response, done);
  }
}

// 发送 RPC 响应给客户端
void RpcServer::SendRpcResponse(const muduo::net::TcpConnectionPtr& conn,
                                 google::protobuf::Message* response) {
  std::string response_str;
  if (response->SerializeToString(&response_str)) {
    // 构造响应：[4 bytes Total Len] + [Response Data]
    uint32_t len = response_str.size();
    uint32_t net_len = htonl(len);

    std::string send_buf;
    send_buf.resize(4 + len);

    std::memcpy(&send_buf[0], &net_len, 4);
    std::memcpy(&send_buf[4], response_str.data(), len);

    conn->send(send_buf);
  } else {
    std::cout << "serialize response error!" << std::endl;
  }
}

// 析构函数，退出事件循环
RpcServer::~RpcServer() {
  std::cout << "~RpcServer()" << std::endl;
  event_loop.quit();
}
