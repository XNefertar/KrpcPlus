#ifndef KRPC_REGISTRY_ROUTE_MANAGER_H_
#define KRPC_REGISTRY_ROUTE_MANAGER_H_

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// 前向声明
class ZookeeperClient;

// 本地路由表管理器（单例模式）
// 使用 RCU（Read-Copy-Update）模式实现无锁读取
class RouteManager {
  using RouteTable =
      std::unordered_map<std::string, std::vector<std::string>>;

 private:
  RouteManager() : _curRouteTable(std::make_shared<RouteTable>()) {}
  std::shared_ptr<const RouteTable> _curRouteTable;
  ZookeeperClient* _zkClient;
  std::mutex _writeMutex;

 public:
  std::vector<std::string> GetRouteNodes(const std::string& path) const;
  void UpdateRouteTable(const std::string& path, ZookeeperClient* client,
                        bool watch = false);
  static RouteManager* GetInstance();
};

#endif  // KRPC_REGISTRY_ROUTE_MANAGER_H_
