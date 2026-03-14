#include <atomic>
#include "krpc/registry/KrpcRouteManager.h"
#include "krpc/common/KrpcLogger.h"

std::vector<std::string> RouteManager::GetRouteNodes(const std::string& path) const {
    std::shared_ptr<const RouteTable> local_table = std::atomic_load(&_curRouteTable);
    const auto it = local_table->find(path);
    if (it != local_table->end()) {
        return it->second;
    }
    return {};
}

void RouteManager::UpdateRouteTable(const std::string& path, ZkClient* client, bool watch) {
    LOG(INFO) << "Updating route for " << path;
    if (!client) {
        LOG(ERROR) << "ZkClient is null";
        return;
    }
    std::vector<std::string> newNodes = client->GetChildren(path.c_str(), watch);
    std::lock_guard<std::mutex> lock(_writeMutex);
    auto oldTable = std::atomic_load(&_curRouteTable);
    auto newTable = std::make_shared<RouteTable>(*oldTable);

    (*newTable)[path] = newNodes;
    std::atomic_store(&_curRouteTable, std::shared_ptr<const RouteTable>(newTable));
}

RouteManager* RouteManager::GetInstance() {
    static RouteManager routeManager;
    return &routeManager;
}