#ifndef _KrpcRouteManager_H__
#define _KrpcRouteManager_H__
#include <mutex>
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include "zookeeperutil.h"

class RouteManager {
    using RouteTable = std::unordered_map<std::string, std::vector<std::string>>;

private:
    RouteManager() : _curRouteTable(std::make_shared<RouteTable>()) {}
    std::shared_ptr<const RouteTable> _curRouteTable;;
    ZkClient *_zkClient;
    std::mutex _writeMutex;

    std::vector<std::string> convertToStringVector(const String_vector& children) {
        std::vector<std::string> routesSet;
        if (children.count > 0) {
            routesSet.reserve(children.count);
        }
        for (int i = 0; i < children.count; ++i) {
            routesSet.push_back(std::string(children.data[i]));
        }
        return routesSet;
    }

public:
    std::vector<std::string> GetRouteNodes(const std::string& path) const;
    void UpdateRouteTable(const std::string& path, ZkClient* client, bool watch = false);
    static RouteManager* GetInstance();
};

#endif