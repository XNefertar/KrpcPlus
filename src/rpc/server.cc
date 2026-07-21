#include "xrpc/rpc/server.h"

#include <iostream>

#include "xrpc/codec/codec_factory.h"
#include "xrpc/codec/noncontiguous_buffer.h"
#include "xrpc/codec/protocol_message.h"
#include "xrpc/codec/xrpc_codec.h"
#include "xrpc/common/application.h"
#include "xrpc/common/logger.h"
#include "xrpc/registry/zookeeper_client.h"

// Muduo TcpConnection 到 Codec Connection 的适配器
// Codec 层不需要访问 Muduo 细节，只把指针原样透传
struct MuduoConnection : public Connection {
  muduo::net::TcpConnectionPtr muduo_conn;
  explicit MuduoConnection(muduo::net::TcpConnectionPtr c)
      : muduo_conn(std::move(c)) {}
};

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
// 使用 Codec 层统一处理 Legacy / XRpc 两种协议
void RpcServer::OnMessage(const muduo::net::TcpConnectionPtr& conn,
                           muduo::net::Buffer* buffer,
                           muduo::Timestamp receive_time) {
  // ---- 步骤 1: 首次收到消息时，自动检测协议并创建 Codec ----
  if (!_serverCodec) {
    NoncontiguousBuffer probe(buffer->peek(), buffer->readableBytes());
    _serverCodec = CodecFactory::CreateServerCodec(probe);
    LOG(INFO) << "ServerCodec created (auto-detected protocol)";
  }

  // ---- 步骤 2: 连接适配器 ----
  auto conn_adapter = std::make_shared<MuduoConnection>(conn);

  // ---- 步骤 3: 循环处理粘包 ----
  while (buffer->readableBytes() > 0) {
    NoncontiguousBuffer nbuf(buffer->peek(), buffer->readableBytes());

    // 3a. 检查是否有完整帧
    std::any metadata;
    int frame_size = _serverCodec->CheckAndPick(conn_adapter, nbuf, metadata);

    if (frame_size == 0) {
      break;  // 半包，等待更多数据
    }
    if (frame_size < 0) {
      LOG(ERROR) << "Invalid frame detected, code=" << frame_size
                 << ", closing connection";
      conn->shutdown();
      return;
    }

    // 3b. 解码完整帧 → ProtocolMessage
    ProtocolMessage msg;
    int decode_ret = _serverCodec->ZeroCopyDecode(conn_adapter, nbuf, msg);
    if (decode_ret != kCodecOk) {
      LOG(ERROR) << "Decode failed: " << decode_ret;
      conn->shutdown();
      return;
    }

    // 3c. 同步 Muduo Buffer（消耗已处理的字节）
    buffer->retrieve(frame_size);

    // ---- 步骤 4: 业务分发（与原逻辑一致） ----
    const std::string& service_name = msg.service_name;
    const std::string& method_name  = msg.method_name;

    auto it = service_map.find(service_name);
    if (it == service_map.end()) {
      std::cout << service_name << " is not exist!" << std::endl;
      continue;  // 不认识的 service，跳过这个包继续处理下一个
    }
    auto mit = it->second.method_map.find(method_name);
    if (mit == it->second.method_map.end()) {
      std::cout << service_name << "." << method_name << " is not exist!"
                << std::endl;
      continue;
    }

    google::protobuf::Service* service = it->second.service;
    const google::protobuf::MethodDescriptor* method = mit->second;

    // 4a. 反序列化请求体
    google::protobuf::Message* request =
        service->GetRequestPrototype(method).New();
    if (!request->ParseFromString(msg.body)) {
      std::cout << "request parse error" << std::endl;
      delete request;
      continue;
    }

    // 4b. 新建响应对象
    google::protobuf::Message* response =
        service->GetResponsePrototype(method).New();

    // 4c. 异步回调：业务执行完毕后调用 SendRpcResponse 编码并发送
    google::protobuf::Closure* done = google::protobuf::NewCallback<
        RpcServer, const muduo::net::TcpConnectionPtr&,
        google::protobuf::Message*>(this, &RpcServer::SendRpcResponse, conn,
                                     response);

    // 4d. 调用业务方法（同步执行在当前 IO 线程）
    service->CallMethod(method, nullptr, request, response, done);
  }
}

// 发送 RPC 响应给客户端（使用 Codec 层编码）
void RpcServer::SendRpcResponse(const muduo::net::TcpConnectionPtr& conn,
                                 google::protobuf::Message* response) {
  std::string response_str;
  if (!response->SerializeToString(&response_str)) {
    std::cout << "serialize response error!" << std::endl;
    return;
  }

  // 构建 ProtocolMessage
  ProtocolMessage resp_msg;
  resp_msg.body         = response_str;
  resp_msg.message_type = MessageType::kResponse;

  // 通过 Codec 编码
  NoncontiguousBuffer out;
  auto conn_adapter = std::make_shared<MuduoConnection>(conn);
  int ret = _serverCodec->ZeroCopyEncode(conn_adapter, resp_msg, out);

  if (ret != kCodecOk) {
    std::cout << "encode response error: " << ret << std::endl;
    return;
  }

  conn->send(out.ToString());
}

// 析构函数，退出事件循环
RpcServer::~RpcServer() {
  std::cout << "~RpcServer()" << std::endl;
  event_loop.quit();
}
