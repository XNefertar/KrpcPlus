#ifndef _KrpcLoadBalancer_H__
#define _KrpcLoadBalancer_H__

#include <vector>
#include <string>
#include <atomic>
#include <random>
#include "KrpcRouteManager.h"

class LoadBalancer {
public:
    virtual ~LoadBalancer() = default;
    virtual RouteNode Select(const std::vector<RouteNode>& nodes) = 0;
};

class RoundRobinLoadBalancer : public LoadBalancer {
private:
    std::atomic<size_t> _index{0};
public:
    RouteNode Select(const std::vector<RouteNode>& nodes) override;
};

class RandomLoadBalancer : public LoadBalancer {
public:
    RouteNode Select(const std::vector<RouteNode>& nodes) override;
};

#endif