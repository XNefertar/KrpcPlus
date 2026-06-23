#ifndef _KrpcLoadBalancer_H__
#define _KrpcLoadBalancer_H__

#include <vector>
#include <string>
#include <atomic>
#include <random>

class LoadBalancer {
public:
    virtual ~LoadBalancer() = default;
    virtual std::string Select(const std::vector<std::string>& nodes) = 0;
};

class RoundRobinLoadBalancer : public LoadBalancer {
private:
    std::atomic<size_t> _index{0};
public:
    std::string Select(const std::vector<std::string>& nodes) override;
};

class RandomLoadBalancer : public LoadBalancer {
public:
    std::string Select(const std::vector<std::string>& nodes) override;
};

#endif