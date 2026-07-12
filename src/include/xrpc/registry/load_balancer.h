#ifndef XRPC_REGISTRY_LOAD_BALANCER_H_
#define XRPC_REGISTRY_LOAD_BALANCER_H_

#include <atomic>
#include <random>
#include <string>
#include <vector>

// 负载均衡抽象基类
class LoadBalancer {
 public:
  virtual ~LoadBalancer() = default;
  virtual std::string Select(const std::vector<std::string>& nodes) = 0;
};

// 轮询负载均衡
class RoundRobinLoadBalancer : public LoadBalancer {
 private:
  std::atomic<size_t> _index{0};

 public:
  std::string Select(const std::vector<std::string>& nodes) override;
};

// 随机负载均衡
class RandomLoadBalancer : public LoadBalancer {
 public:
  std::string Select(const std::vector<std::string>& nodes) override;
};

#endif  // XRPC_REGISTRY_LOAD_BALANCER_H_
