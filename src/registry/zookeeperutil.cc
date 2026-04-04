#include <mutex>
#include <vector>
#include <condition_variable>
#include "krpc/registry/zookeeperutil.h"
#include "krpc/common/Krpcapplication.h"
#include "krpc/common/KrpcLogger.h"
#include "krpc/registry/KrpcRouteManager.h"

// 全局的watcher观察器，用于接收ZooKeeper服务器的通知
void global_watcher(zhandle_t *zh, int type, int status, const char *path, void *watcherCtx) {
    if (type == ZOO_SESSION_EVENT) {  // 回调消息类型和会话相关的事件
        if (status == ZOO_CONNECTED_STATE) {  // ZooKeeper客户端和服务器连接成功
            ZkClient *client = static_cast<ZkClient*>(watcherCtx);
            if (client) {
                std::lock_guard<std::mutex> lock(client->cv_mutex);  // 加锁保护
                client->is_connected = true;  // 标记连接成功
                client->cv.notify_all();  // 通知所有等待的线程
            }
        } else if (status == ZOO_EXPIRED_SESSION_STATE) {
            LOG(FATAL) << "ZK session expired! Need to re-init...";
        }
    } else if (type == ZOO_CHILD_EVENT) {
        if (path != nullptr) {
            LOG(INFO) << "ZOO_CHILD_EVENT triggered for path: " << path;
            ZkClient *client = static_cast<ZkClient*>(watcherCtx);
            RouteManager::GetInstance()->UpdateRouteTable(std::string(path), client, true);
        }
    }
}

// 构造函数，初始化ZooKeeper客户端句柄为空
ZkClient::ZkClient() : m_zhandle(nullptr) {}

// 析构函数，关闭ZooKeeper连接
ZkClient::~ZkClient() {
    if (m_zhandle != nullptr) {
        zookeeper_close(m_zhandle);  // 关闭ZooKeeper连接
    }
}

// 启动ZooKeeper客户端，连接ZooKeeper服务器
void ZkClient::Start() {
    // 设置 ZooKeeper 库的日志级别，过滤掉 INFO 级别
    // ZOO_LOG_LEVEL_ERROR (仅错误), ZOO_LOG_LEVEL_WARN (警告+错误), ZOO_LOG_LEVEL_INFO (默认)
    zoo_set_debug_level(ZOO_LOG_LEVEL_WARN);

    // 从配置文件中读取ZooKeeper服务器的IP和端口
    std::string host = KrpcApplication::GetInstance().GetConfig().Load("zookeeperip");
    std::string port = KrpcApplication::GetInstance().GetConfig().Load("zookeeperport");
    std::string connstr = host + ":" + port;  // 拼接连接字符串

    /*
    zookeeper_mt：多线程版本
    ZooKeeper的API客户端程序提供了三个线程：
    1. API调用线程
    2. 网络I/O线程（使用pthread_create和poll）
    3. watcher回调线程（使用pthread_create）
    */

    // 使用zookeeper_init初始化一个ZooKeeper客户端对象，异步建立与服务器的连接
    // 第四个参数传入this，以便watcher能够通知对应的对象
    m_zhandle = zookeeper_init(connstr.c_str(), global_watcher, 6000, nullptr, this, 0);
    if (nullptr == m_zhandle) {  // 初始化失败
        LOG(ERROR) << "zookeeper_init error";
        exit(EXIT_FAILURE);  // 退出程序
    }

    // 等待连接成功
    std::unique_lock<std::mutex> lock(cv_mutex);
    cv.wait(lock, [this] { return is_connected; });  // 阻塞等待，直到连接成功
    LOG(INFO) << "zookeeper_init success";  // 记录日志，表示连接成功
}

// 创建ZooKeeper节点
void ZkClient::Create(const char *path, const char *data, int datalen, int state) {
    char path_buffer[128];  // 用于存储创建的节点路径
    int bufferlen = sizeof(path_buffer);

    // 检查节点是否已经存在
    int flag = zoo_exists(m_zhandle, path, 0, nullptr);
    if (flag == ZNONODE) {  // 如果节点不存在
        // 创建指定的ZooKeeper节点
        flag = zoo_create(m_zhandle, path, data, datalen, &ZOO_OPEN_ACL_UNSAFE, state, path_buffer, bufferlen);
        if (flag == ZOK) {  // 创建成功
            LOG(INFO) << "znode create success... path:" << path;
        } else {  // 创建失败
            LOG(ERROR) << "znode create failed... path:" << path;
            exit(EXIT_FAILURE);  // 退出程序
        }
    }
}

// 获取ZooKeeper节点的数据
std::string ZkClient::GetData(const char *path) {
    char buf[64];  // 用于存储节点数据
    int bufferlen = sizeof(buf);

    // 获取指定节点的数据
    int flag = zoo_get(m_zhandle, path, 0, buf, &bufferlen, nullptr);
    if (flag != ZOK) {  // 获取失败
        LOG(ERROR) << "zoo_get error";
        return "";  // 返回空字符串
    } else {  // 获取成功
        return buf;  // 返回节点数据
    }
    return "";  // 默认返回空字符串
}

std::vector<RouteNode> ZkClient::GetChildren(const char* path, bool watch) {
    String_vector children;
    int flag = zoo_get_children(m_zhandle, path, watch, &children);
    if (flag != ZOK) {
        LOG(ERROR) << "zoo_get_children for path [" << path << "] error!";
        return {};
    }
    std::vector<RouteNode> routeNodes;
    if (children.count > 0) {
        routeNodes.reserve(children.count);
        for (int i = 0; i < children.count; ++i) {
            routeNodes.emplace_back(children.data[i], Normal);
        }
    }
    deallocate_String_vector(&children);
    return routeNodes;
}