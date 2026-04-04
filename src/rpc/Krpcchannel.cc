#include "Krpcchannel.h"
#include "Krpcheader.pb.h"
#include "zookeeperutil.h"
#include "Krpcapplication.h"
#include "Krpccontroller.h"
#include "KrpcRouteManager.h"
#include "KrpcLoadBalancer.h"
#include "KrpcStat.h"
#include "memory"
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include "KrpcLogger.h"

std::mutex g_data_mutx;  // 全局互斥锁，用于保护共享数据的线程安全


// 辅助函数：循环读取直到读够 size 字节
ssize_t KrpcChannel::recv_exact(int fd, char* buf, size_t size) {
    size_t total_read = 0;
    while (total_read < size) {
        ssize_t ret = recv(fd, buf + total_read, size - total_read, 0);
        if (ret == 0) return 0; // 对端关闭
        if (ret == -1) {
            if (errno == EINTR) continue; // 中断信号，继续读
            return -1; // 错误
        }
        total_read += ret;
    }
    return total_read;
}

// RPC调用的核心方法，负责将客户端的请求序列化并发送到服务端，同时接收服务端的响应
void KrpcChannel::CallMethod(const ::google::protobuf::MethodDescriptor *method,
                             ::google::protobuf::RpcController *controller,
                             const ::google::protobuf::Message *request,
                             ::google::protobuf::Message *response,
                             ::google::protobuf::Closure *done)
{
    ScopedTimer total(KrpcStat::TOTAL);
    std::string zk_path, address;
    
    // 获取服务对象名和方法名
    const google::protobuf::ServiceDescriptor *sd = method->service();
    service_name = sd->name();
    method_name = method->name();

    // 1. 序列化请求参数
    std::string args_str;
    {
        ScopedTimer ser(KrpcStat::SERIALIZE_REQ);
        if (!request->SerializeToString(&args_str)) {
            controller->SetFailed("serialize request fail");
            return;
        }
    }

    // 2. 构建协议头
    Krpc::RpcHeader krpcheader;
    krpcheader.set_service_name(service_name);
    krpcheader.set_method_name(method_name);
    krpcheader.set_args_size(args_str.size());

    std::string rpc_header_str;
    if (!krpcheader.SerializeToString(&rpc_header_str)) {
        controller->SetFailed("serialize rpc header error!");
        return;
    }

    // 3. 打包数据发送
    uint32_t header_size = rpc_header_str.size();
    uint32_t total_len = 4 + header_size + args_str.size(); 
    
    uint32_t net_total_len = htonl(total_len);
    uint32_t net_header_len = htonl(header_size);

    std::string send_rpc_str;
    send_rpc_str.reserve(4 + 4 + header_size + args_str.size());
    send_rpc_str.append((char*)&net_total_len, 4);
    send_rpc_str.append((char*)&net_header_len, 4);
    send_rpc_str.append(rpc_header_str);
    send_rpc_str.append(args_str);

    int retry_count = 3;
    bool call_success = false;

    while (retry_count > 0 && !call_success) {
        if (-1 == m_clientfd) {  // 如果客户端socket未初始化
            // 获取持久存活的全局 ZkClient
            ZkClient* globalZkCli = RouteManager::GetInstance()->GetZkClient();
            RouteNode hostNode;
            {
                ScopedTimer zk(KrpcStat::ZK_QUERY);
                // 传入持久存活的 client
                hostNode = QueryServiceHost(globalZkCli, service_name, method_name);
            }

            if (!hostNode.isAlive()) {
                LOG(ERROR) << "no alive host node, retry...";
                retry_count--;
                continue;
            }

            m_ip = hostNode.FetchIPAddress();
            m_port = atoi(hostNode.FetchPort().c_str());
            zk_path = "/" + service_name + "/" + method_name;
            address = hostNode.FetchAddress();

            // 尝试连接服务器
            auto rt = newConnect(m_ip.c_str(), m_port);
            if (!rt) {
                RouteManager::GetInstance()->MarkNodeStatus(zk_path, address, Error);
                LOG(ERROR) << "connect server error, retry...";
                m_clientfd = -1;
                retry_count--;
                continue;
            } else {
                LOG(INFO) << "connect server success";
            }
        }

        // 发送
        {
            ScopedTimer io(KrpcStat::NET_IO);
            if (-1 == send(m_clientfd, send_rpc_str.c_str(), send_rpc_str.size(), 0)) {
                close(m_clientfd);
                m_clientfd = -1; // 重置
                RouteManager::GetInstance()->MarkNodeStatus(zk_path, address, Error);
                LOG(ERROR) << "send error, retry...";
                retry_count--;
                continue;
            }

            // A. 先读4字节长度头
            uint32_t response_len = 0;
            if (recv_exact(m_clientfd, (char*)&response_len, 4) != 4) {
                close(m_clientfd);
                m_clientfd = -1;
                RouteManager::GetInstance()->MarkNodeStatus(zk_path, address, Error);
                LOG(ERROR) << "recv response length error, retry...";
                retry_count--;
                continue;
            }
            response_len = ntohl(response_len); // 转回主机字节序

            // B. 根据长度读取Body
            std::vector<char> recv_buf(response_len);
            if (recv_exact(m_clientfd, recv_buf.data(), response_len) != response_len) {
                close(m_clientfd);
                m_clientfd = -1;
                RouteManager::GetInstance()->MarkNodeStatus(zk_path, address, Error);
                LOG(ERROR) << "recv response body error, retry...";
                retry_count--;
                continue;
            }
            
            // 6. 反序列化响应
            {
                ScopedTimer des(KrpcStat::DESERIALIZE_RES);
                if (!response->ParseFromArray(recv_buf.data(), response_len)) {
                    close(m_clientfd);
                    m_clientfd = -1;
                    controller->SetFailed("parse response error");
                    return;
                }
            }
        }

        // 调用成功
        call_success = true;
    }

    if (!call_success && !controller->Failed()) {
        controller->SetFailed("RPC call failed after multiple retries");
    }
}

// 创建新的socket连接
bool KrpcChannel::newConnect(const char *ip, uint16_t port) {
    // 创建socket
    int clientfd = socket(AF_INET, SOCK_STREAM, 0);
    if (-1 == clientfd) {
        char errtxt[512] = {0};
        std::cout << "socket error" << strerror_r(errno, errtxt, sizeof(errtxt)) << std::endl;  // 打印错误信息
        LOG(ERROR) << "socket error:" << errtxt;  // 记录错误日志
        return false;
    }

    // 设置服务器地址信息
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;  // IPv4地址族
    server_addr.sin_port = htons(port);  // 端口号
    server_addr.sin_addr.s_addr = inet_addr(ip);  // IP地址

    // 尝试连接服务器
    if (-1 == connect(clientfd, (struct sockaddr *)&server_addr, sizeof(server_addr))) {
        close(clientfd);  // 连接失败，关闭socket
        char errtxt[512] = {0};
        std::cout << "connect error" << strerror_r(errno, errtxt, sizeof(errtxt)) << std::endl;  // 打印错误信息
        LOG(ERROR) << "connect server error" << errtxt;  // 记录错误日志
        return false;
    }

    m_clientfd = clientfd;  // 保存socket文件描述符
    return true;
}

// 从ZooKeeper查询服务地址
RouteNode KrpcChannel::QueryServiceHost(ZkClient *zkclient, std::string service_name, std::string method_name) {
    std::string method_path = "/" + service_name + "/" + method_name;  // 构造ZooKeeper路径
    std::cout << "method_path: " << method_path << std::endl;

    std::unique_lock<std::mutex> lock(g_data_mutx);  // 加锁，保证线程安全
    
    // 我们需要先触发一次对 ZK 的查询来更新本地路由表，或者发现本地没有时强制去ZK拉取
    std::vector<RouteNode> routeNodes = RouteManager::GetInstance()->GetHealthNodes(method_path);
    if (routeNodes.empty()) {
        // 如果本地路由表没有缓存，尝试同步去ZK拉取一次并注册Watch
        RouteManager::GetInstance()->UpdateRouteTable(method_path, zkclient, true);
        routeNodes = RouteManager::GetInstance()->GetHealthNodes(method_path);
    }
    if (routeNodes.empty()) return {};
    
    // 获取全局单例负载均衡器并选择节点
    RouteNode hostNode = RouteManager::GetInstance()->GetLoadBalancer()->Select(routeNodes);
    lock.unlock();  // 解锁

    if (hostNode.FetchAddress() == "") {  // 如果未找到服务地址
        LOG(ERROR) << method_path + " is not exist!";  // 记录错误日志
        return {};
    }

    if (!hostNode.isAddressDivisible()) {  // 如果分隔符不存在
        LOG(ERROR) << method_path + " address is invalid!";  // 记录错误日志
        return {};
    }

    return hostNode;  // 返回服务地址
}

// 构造函数，支持延迟连接
KrpcChannel::KrpcChannel(bool connectNow) : m_clientfd(-1) {
    if (!connectNow) {  // 如果不需要立即连接
        return;
    }

    // 尝试连接服务器，最多重试3次
    auto rt = newConnect(m_ip.c_str(), m_port);
    int count = 3;  // 重试次数
    while (!rt && count--) {
        rt = newConnect(m_ip.c_str(), m_port);
    }

}