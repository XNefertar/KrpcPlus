#ifndef KRPC_REGISTRY_ZOOKEEPER_CLIENT_H_
#define KRPC_REGISTRY_ZOOKEEPER_CLIENT_H_

#include <zookeeper/zookeeper.h>

#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>

// ZooKeeper 客户端封装，负责连接 ZK 服务器、创建/查询节点
class ZookeeperClient {
 public:
  ZookeeperClient();
  ~ZookeeperClient();

  // 连接 ZK 服务器
  void Start();

  // 在 ZK 中创建一个节点
  void Create(const char* path, const char* data, int datalen, int state = 0);

  // 获取指定 znode 节点的值
  std::string GetData(const char* path);

  // 获取指定节点的子节点列表
  std::vector<std::string> GetChildren(const char* path, bool watch = false);

  std::mutex cv_mutex;
  std::condition_variable cv;
  bool is_connected = false;

 private:
  // ZK 客户端句柄
  zhandle_t* m_zhandle;
};

#endif  // KRPC_REGISTRY_ZOOKEEPER_CLIENT_H_
