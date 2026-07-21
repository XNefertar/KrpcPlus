#include <mutex>
#include <vector>
#include <condition_variable>

#include "xrpc/registry/zookeeper_client.h"
#include "xrpc/common/application.h"
#include "xrpc/common/logger.h"
#include "xrpc/registry/route_manager.h"

// 全局的 watcher 观察器，用于接收 ZooKeeper 服务器的通知
void global_watcher(zhandle_t* zh, int type, int status, const char* path,
                    void* watcherCtx) {
  if (type == ZOO_SESSION_EVENT) {
    if (status == ZOO_CONNECTED_STATE) {
      ZookeeperClient* client = static_cast<ZookeeperClient*>(watcherCtx);
      if (client) {
        std::lock_guard<std::mutex> lock(client->cv_mutex);
        client->is_connected = true;
        client->cv.notify_all();
      }
    } else if (status == ZOO_EXPIRED_SESSION_STATE) {
      LOG(FATAL) << "ZK session expired! Need to re-init...";
    }
  } else if (type == ZOO_CHILD_EVENT) {
    if (path != nullptr) {
      LOG(INFO) << "ZOO_CHILD_EVENT triggered for path: " << path;
      ZookeeperClient* client = static_cast<ZookeeperClient*>(watcherCtx);
      RouteManager::GetInstance()->UpdateRouteTable(std::string(path), client,
                                                     true);
    }
  }
}

// 构造函数，初始化 ZK 客户端句柄为空
ZookeeperClient::ZookeeperClient() : m_zhandle(nullptr) {}

// 析构函数，关闭 ZK 连接
ZookeeperClient::~ZookeeperClient() {
  if (m_zhandle != nullptr) {
    zookeeper_close(m_zhandle);
  }
}

// 启动 ZK 客户端，连接 ZK 服务器
void ZookeeperClient::Start() {
  // 设置 ZK 库的日志级别
  zoo_set_debug_level(ZOO_LOG_LEVEL_WARN);

  // 从配置文件中读取 ZK 服务器的 IP 和端口
  std::string host =
      Application::GetInstance().GetConfig().Load("zookeeperip");
  std::string port =
      Application::GetInstance().GetConfig().Load("zookeeperport");
  std::string connstr = host + ":" + port;

  /*
  zookeeper_mt：多线程版本
  ZK 的 API 客户端程序提供了三个线程：
  1. API 调用线程
  2. 网络 I/O 线程（使用 pthread_create 和 poll）
  3. watcher 回调线程（使用 pthread_create）
  */

  m_zhandle =
      zookeeper_init(connstr.c_str(), global_watcher, 6000, nullptr, this, 0);
  if (nullptr == m_zhandle) {
    LOG(ERROR) << "zookeeper_init error";
    exit(EXIT_FAILURE);
  }

  // 等待连接成功
  std::unique_lock<std::mutex> lock(cv_mutex);
  cv.wait(lock, [this] { return is_connected; });
  LOG(INFO) << "zookeeper_init success";
}

// 创建 ZK 节点
void ZookeeperClient::Create(const char* path, const char* data, int datalen,
                              int state) {
  char path_buffer[128];
  int bufferlen = sizeof(path_buffer);

  // 检查节点是否已经存在
  int flag = zoo_exists(m_zhandle, path, 0, nullptr);
  if (flag == ZNONODE) {
    flag = zoo_create(m_zhandle, path, data, datalen, &ZOO_OPEN_ACL_UNSAFE,
                      state, path_buffer, bufferlen);
    if (flag == ZOK) {
      LOG(INFO) << "znode create success... path:" << path;
    } else {
      LOG(ERROR) << "znode create failed... path:" << path;
      exit(EXIT_FAILURE);
    }
  }
}

// 获取 ZK 节点的数据
std::string ZookeeperClient::GetData(const char* path) {
  char buf[64];
  int bufferlen = sizeof(buf);

  int flag = zoo_get(m_zhandle, path, 0, buf, &bufferlen, nullptr);
  if (flag != ZOK) {
    LOG(ERROR) << "zoo_get error";
    return "";
  } else {
    return buf;
  }
}

std::vector<std::string> ZookeeperClient::GetChildren(const char* path,
                                                       bool watch) {
  String_vector children;
  int flag = zoo_get_children(m_zhandle, path, watch, &children);
  if (flag != ZOK) {
    LOG(ERROR) << "zoo_get_children for path [" << path << "] error!";
    return {};
  }
  std::vector<std::string> routeNodes;
  if (children.count > 0) {
    routeNodes.reserve(children.count);
    for (int i = 0; i < children.count; ++i) {
      routeNodes.emplace_back(children.data[i]);
    }
  }
  deallocate_String_vector(&children);
  return routeNodes;
}
