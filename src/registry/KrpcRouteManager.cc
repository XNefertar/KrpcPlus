#include <atomic>
#include <iostream>
#include "krpc/registry/KrpcRouteManager.h"
#include "krpc/registry/zookeeperutil.h"
#include "krpc/registry/KrpcLoadBalancer.h"
#include "krpc/common/KrpcLogger.h"
#include "krpc/common/Krpcapplication.h"

RouteManager::RouteManager() : _curRouteTable(std::make_shared<RouteTable>()) {
    std::string lb_type = KrpcApplication::GetConfig().Load("loadbalancer");
    if (lb_type == "roundrobin") {
        _loadBalancer = std::make_unique<RoundRobinLoadBalancer>();
    } else {
        _loadBalancer = std::make_unique<RandomLoadBalancer>();
    }
    _zkClient = new ZkClient();
    _zkClient->Start();

    _healthCheck = std::thread(&RouteManager::HealthCheckTask, this);
    std::cout << "HealthCheckThread start" << std::endl;
    _healthCheck.detach();
}

std::vector<RouteNode> RouteManager::GetRouteNodes(const std::string& path) const {
    std::shared_ptr<const RouteTable> local_table = std::atomic_load(&_curRouteTable);
    const auto it = local_table->find(path);
    if (it != local_table->end()) {
        return it->second;
    }
    return {};
}

std::vector<RouteNode> RouteManager::GetHealthNodes(const std::string& path) const {
    std::shared_ptr<const RouteTable> local_table = std::atomic_load(&_curRouteTable);
    auto it = local_table->find(path);
    if (it != local_table->end()) {
        std::vector<RouteNode> healthNodes;
        for (const auto& node : it->second) {
            if (node.isAlive()) {
                healthNodes.push_back(node);
            }
        }
        return healthNodes;
    }
    return {};
}

void RouteManager::UpdateRouteTable(const std::string& path, ZkClient* client, bool watch) {
    LOG(INFO) << "Updating route for " << path;
    if (!client) {
        LOG(ERROR) << "ZkClient is null";
        return;
    }
    std::vector<RouteNode> newNodes = client->GetChildren(path.c_str(), watch);
    std::lock_guard<std::mutex> lock(_writeMutex);
    auto oldTable = std::atomic_load(&_curRouteTable);
    auto newTable = std::make_shared<RouteTable>(*oldTable);

    // 状态继承：保留旧节点的现有状态（如果是Error，仍然靠HealthCheck和重试去翻转；如果是Normal则继承保活）
    // 对于全新发现的节点，由于ZK上的心跳存在，其初始为Normal
    auto oldIt = oldTable->find(path);
    if (oldIt != oldTable->end()) {
        for (auto& newNode : newNodes) {
            newNode._status = Normal; // 确立全新上线节点默认即可见，立即承接流量
            for (const auto& oldNode : oldIt->second) {
                if (oldNode._address == newNode._address) {
                    newNode._status = oldNode._status; // 继承旧状态
                    break;
                }
            }
        }
    } else {
        // 全新路径，全部默认可见
        for (auto& newNode : newNodes) {
             newNode._status = Normal;
        }
    }

    (*newTable)[path] = newNodes;
    std::atomic_store(&_curRouteTable, std::shared_ptr<const RouteTable>(newTable));
}

void RouteManager::MarkNodeStatus(const std::string& path, const std::string& address, NODE_STATUS target_status) {
    LOG(INFO) << "Marking Node for address" << address;
    std::cout << "Marking Node for address" << address << std::endl;
    // 1. 无锁快读：先用 atomic_load 检查是否真的需要更新
    auto local_table = std::atomic_load(&_curRouteTable);
    auto it = local_table->find(path);
    if (it == local_table->end()) return;

    bool needUpdate = false;
    for (const auto& node : it->second) {
        if (node._address == address && node._status != target_status) {
            needUpdate = true;
            break;
        }
    }
    // 如果状态早已被其他线程改对了，直接立刻返回，完全避开锁和冗余拷贝
    if (!needUpdate) return; 

    // 2. 确实需要更新，进入临界区
    std::lock_guard<std::mutex> lock(_writeMutex);
    
    // 【致命缺陷修复】：加锁后必须"重新读取"最新状态！
    // 因为在等待锁的时间里，其他线程（如 Zookeeper更新线程）可能已经修改了 _curRouteTable
    auto oldTable = std::atomic_load(&_curRouteTable);
    if (oldTable->find(path) == oldTable->end()) return;

    // 3. 在完全安全的临界区内完成 拷贝-修改-替换 (Copy-On-Write)
    auto newTable = std::make_shared<RouteTable>(*oldTable);
    auto& nodes = (*newTable)[path];

    bool isChanged = false;
    for (auto& node : nodes) {
        if (node._address == address && node._status != target_status) {
            node._status = target_status;
            isChanged = true;
            break;
        }
    }
    
    if (isChanged) {
        std::atomic_store(&_curRouteTable, std::shared_ptr<const RouteTable>(newTable));
        LOG(INFO) << "Node " << address << " status changed to " << (target_status == Error ? "Error" : "Normal");
    }
}

RouteManager* RouteManager::GetInstance() {
    static RouteManager routeManager;
    return &routeManager;
}