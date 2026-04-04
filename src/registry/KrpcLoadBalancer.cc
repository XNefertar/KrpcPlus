#include "krpc/registry/KrpcLoadBalancer.h"
#include "krpc/registry/KrpcRouteManager.h"

RouteNode RoundRobinLoadBalancer::Select(const std::vector<RouteNode>& nodes) {
    if (nodes.empty()) {
        return {};
    }
    size_t index = _index.fetch_add(1) % nodes.size();
    return nodes[index];
}

RouteNode RandomLoadBalancer::Select(const std::vector<RouteNode>& nodes) {
    if (nodes.empty()) {
        return {};
    }
    thread_local static std::mt19937 generator(std::random_device{}());
    std::uniform_int_distribution<size_t> distribution(0, nodes.size() - 1);
    return nodes[distribution(generator)];
}